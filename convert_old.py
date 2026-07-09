#!/usr/bin/env python3
"""
将 pbb_old.cpp 的旧输入格式 (input_daily.txt) 转换为新的 YAML 配置文件。

旧格式中 cin>> 读取空白分隔的 token (整数可同行), read() 读取整行。
新格式输出为 config.yaml。

用法:
  python convert_old.py input_old.txt [-o config.yaml]
"""
import sys, os, argparse


# ══════════════════════════════════════════════════════════════════════════════
# 旧格式解析器 — 模拟 C++ cin>>(token) / read()(行) 交替
# ══════════════════════════════════════════════════════════════════════════════

def parse_old(path: str) -> dict:
    """解析旧格式文件, 返回结构化 dict."""
    lines = _read_nonempty_lines(path)

    # 将每一行拆成 tokens (按空白); 同时保留原始行用于 read()
    # 使用游标: ti=token索引(跨行), li=当前行索引
    tokens = []   # (token_str, line_index)
    for li, line in enumerate(lines):
        for tok in line.split():
            tokens.append((tok, li))

    ti = 0  # token 索引

    def next_int() -> int:
        nonlocal ti
        v = int(tokens[ti][0])
        ti += 1
        return v

    def next_line() -> str:
        """读取整行 (模拟 read()). 每行只被消费一次."""
        nonlocal ti
        # 找到当前 token 对应的行, 返回该行的原始内容
        li = tokens[ti][1]
        line = lines[li]
        # 跳过该行上的所有 token (包括空格分隔的子 token)
        while ti < len(tokens) and tokens[ti][1] == li:
            ti += 1
        return line

    result = {}

    # 1. debug_mode
    result["debug_mode"] = next_int()

    # 2. team_name
    result["team_name"] = next_line()

    # 3. prefixes
    prefix_count = next_int()
    prefixes = []
    for _ in range(prefix_count):
        p = next_line()
        if p == "@":
            enc_len = next_int()
            for _ in range(enc_len):
                next_int()
            prefixes.append("")
        else:
            prefixes.append(p)
    result["prefixes"] = prefixes

    # 4. suffixes
    suffix_count = next_int()
    suffixes = []
    for _ in range(suffix_count):
        s = next_line()
        if s == "@":
            enc_len = next_int()
            for _ in range(enc_len):
                next_int()
            suffixes.append("")
        else:
            suffixes.append(s)
    result["suffixes"] = suffixes

    # 5. charset: scl, type_count, types[], ...
    scl = next_int()
    result["scl"] = scl

    type_count = next_int()
    types = [next_int() for _ in range(type_count)]
    result["types"] = types

    # 跳过自定义 / Unicode 数据
    for t in types:
        if _is_diy_type(scl, t):
            diy_len = next_int()
            for _ in range(diy_len * scl):
                next_int()
        elif scl in (2, 3, 4) and t == 8:
            range_cnt = next_int()
            for _ in range(range_cnt):
                next_int()
                next_int()

    # output_charset_type
    next_int()

    # 6. mode
    mode = next_int()
    result["mode"] = mode

    if mode == 1:
        vlen = next_int()
        result["variable_length"] = vlen
        group_count = next_int()
        prefix_ranges = []
        for _ in range(group_count):
            n = next_int()
            l = next_int()
            r = next_int()
            if l < 0:
                l = 0
            if r == -1:
                r = 0          # -1 表示跳过此前缀 (range [0,0] → 零 chunk)
            prefix_ranges.append({"count": n, "start": l, "end": r})
        result["prefix_ranges"] = prefix_ranges
    else:
        vlen = next_int()
        result["variable_length"] = vlen
        r_val = next_int()
        if r_val == -1:
            r_val = 2**64 - 1
        result["random_total"] = r_val

    # 7. settings
    result["xp_min"] = next_int()
    result["xd_min"] = next_int()

    collect = next_int()
    result["collect_mode"] = collect
    if collect == 2:
        result["c_eight_v_min"] = next_int()
        result["c_seven_v_min"] = next_int()
        result["c_hl_min"] = next_int()
        result["c_hp398_min"] = next_int()

    result["output_xp"] = next_int()
    result["output_utf"] = next_int()

    output_log = next_int()
    result["output_log"] = output_log
    if output_log == 0:
        next_line()

    output_speed = next_int()
    result["output_speed"] = output_speed
    if output_speed == 0:
        next_line()

    result["result_file"] = next_line()

    result["worker_threads"] = next_int()

    return result


def _read_nonempty_lines(path: str) -> list[str]:
    """读取文件, 跳过空行."""
    with open(path, "r", encoding="utf-8") as f:
        raw = f.read()
    return [l for l in raw.splitlines() if l.strip() != ""]


def _is_diy_type(scl: int, t: int) -> bool:
    """旧 type 编号中哪些触发 get_diy_charset()?"""
    return (scl == 1 and t in (4,)) or \
           (scl in (2, 3) and t == 7) or \
           (scl == 4 and t == 2) or \
           (scl >= 5 and t == 1)


# ══════════════════════════════════════════════════════════════════════════════
# 新格式构建
# ══════════════════════════════════════════════════════════════════════════════

def build_config(parsed: dict) -> dict:
    """从解析结果构建新格式 config dict."""
    cfg = {}

    cfg["debug_mode"] = parsed["debug_mode"]
    cfg["team_name"] = parsed["team_name"]

    # prefixes
    cfg["prefixes"] = [
        {"name": p if p not in ("+", "") else "+"}
        for p in parsed["prefixes"]
    ]

    # suffixes
    cfg["suffixes"] = [
        {"name": s if s not in ("+", "") else "+"}
        for s in parsed["suffixes"]
    ]

    # character_set
    scl = parsed["scl"]
    cfg["character_set"] = {
        "single_char_length": scl,
        "types": parsed["types"],
    }

    # enumeration
    mode = parsed["mode"]
    en = {
        "mode": mode,
        "variable_length": parsed["variable_length"],
    }
    if "prefix_ranges" in parsed:
        en["prefix_ranges"] = parsed["prefix_ranges"]
        en["ranges"] = [{"start": 0, "end": int(1e18)}]  # 全局默认占位
    else:
        total = parsed.get("random_total", int(1e18))
        en["ranges"] = [{"start": 0, "end": total}]
    cfg["enumeration"] = en

    # collection
    cl = {
        "xp_min": parsed["xp_min"],
        "xd_min": parsed["xd_min"],
        "collect_mode": parsed["collect_mode"],
    }
    if parsed["collect_mode"] == 2:
        cl["special_thresholds"] = {
            "eight_v_min": parsed["c_eight_v_min"],
            "seven_v_min": parsed["c_seven_v_min"],
            "hl_min": parsed["c_hl_min"],
            "hp398_eight_v_min": parsed["c_hp398_min"],
        }
    cfg["collection"] = cl

    # output
    cfg["output"] = {
        "output_xp": parsed["output_xp"],
        "log_output": parsed["output_log"],
        "speed_output": parsed["output_speed"],
        "result_file": parsed["result_file"],
    }

    # threads
    cfg["threads"] = {
        "worker_threads": parsed["worker_threads"],
    }

    return cfg


# ══════════════════════════════════════════════════════════════════════════════
# YAML 序列化 (不依赖 PyYAML)
# ══════════════════════════════════════════════════════════════════════════════

def to_yaml(cfg: dict) -> str:
    def _str(s):
        s = str(s)
        if s == "":
            return '""'
        if any(c in s for c in ':#{}[]&*!|>\'"%@`,'):
            return f"'{s}'"
        if s.startswith("+") or s.startswith("-"):
            return f"'{s}'"
        if s in ("true", "false", "yes", "no", "on", "off", "null", "~"):
            return f"'{s}'"
        # YAML strips trailing spaces from unquoted scalars — force quoting
        if s != s.rstrip() or s != s.lstrip():
            return f"'{s}'"
        return s

    lines = []

    lines.append(f"debug_mode: {cfg['debug_mode']}")
    lines.append(f"team_name: {_str(cfg['team_name'])}")
    lines.append("")

    lines.append("prefixes:")
    for p in cfg["prefixes"]:
        lines.append(f"  - name: {_str(p['name'])}")
    lines.append("")

    lines.append("suffixes:")
    for s in cfg["suffixes"]:
        lines.append(f"  - name: {_str(s['name'])}")
    lines.append("")

    cs = cfg["character_set"]
    lines.append("character_set:")
    lines.append(f"  single_char_length: {cs['single_char_length']}")
    lines.append(f"  types: {cs['types']}")
    if "custom_values" in cs:
        lines.append(f"  custom_values: {_str(cs['custom_values'])}")
    lines.append("")

    en = cfg["enumeration"]
    lines.append("enumeration:")
    lines.append(f"  mode: {en['mode']}")
    lines.append(f"  variable_length: {en['variable_length']}")
    if "prefix_ranges" in en:
        lines.append("  prefix_ranges:")
        for pr in en["prefix_ranges"]:
            lines.append(f"    - count: {pr['count']}")
            lines.append(f"      start: {pr['start']}")
            lines.append(f"      end: {pr['end']}")
    if "ranges" in en:
        lines.append("  ranges:")
        for r in en["ranges"]:
            lines.append(f"    - start: {r['start']}")
            lines.append(f"      end: {r['end']}")
    lines.append("")

    cl = cfg["collection"]
    lines.append("collection:")
    lines.append(f"  xp_min: {cl['xp_min']}")
    lines.append(f"  xd_min: {cl['xd_min']}")
    lines.append(f"  collect_mode: {cl['collect_mode']}")
    if cl["collect_mode"] == 2 and "special_thresholds" in cl:
        st = cl["special_thresholds"]
        lines.append("  special_thresholds:")
        lines.append(f"    eight_v_min: {st['eight_v_min']}")
        lines.append(f"    seven_v_min: {st['seven_v_min']}")
        lines.append(f"    hl_min: {st['hl_min']}")
        lines.append(f"    hp398_eight_v_min: {st['hp398_eight_v_min']}")
    lines.append("")

    out = cfg["output"]
    lines.append("output:")
    lines.append(f"  output_xp: {out['output_xp']}")
    lines.append(f"  log_output: {out['log_output']}")
    lines.append(f"  speed_output: {out['speed_output']}")
    lines.append(f"  result_file: {_str(out['result_file'])}")
    lines.append("")

    th = cfg["threads"]
    lines.append("threads:")
    lines.append(f"  worker_threads: {th['worker_threads']}")

    return "\n".join(lines) + "\n"


# ══════════════════════════════════════════════════════════════════════════════
# 主入口
# ══════════════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="PBB 旧格式 (input_daily.txt) → 新 YAML 配置转换器"
    )
    parser.add_argument("input", help="旧格式输入文件")
    parser.add_argument("-o", "--output", default="config.yaml",
                        help="输出 YAML 路径 (默认: config.yaml)")
    args = parser.parse_args()

    if not os.path.exists(args.input):
        print(f"ERROR: 文件不存在: {args.input}", file=sys.stderr)
        sys.exit(1)

    parsed = parse_old(args.input)
    config = build_config(parsed)

    yaml_str = to_yaml(config)

    with open(args.output, "w", encoding="utf-8") as f:
        f.write(yaml_str)

    pfx_cnt = len(parsed["prefixes"])
    sfx_cnt = len(parsed["suffixes"])
    scl = parsed["scl"]
    types = parsed["types"]

    print(f"转换完成: {args.input} → {args.output}")
    print(f"  前缀数: {pfx_cnt}  后缀数: {sfx_cnt}")
    print(f"  字符集 scl={scl}, types={types}")
    en = config["enumeration"]
    print(f"  mode={en['mode']}, vlen={en['variable_length']}")
    if "prefix_ranges" in en:
        for pr in en["prefix_ranges"]:
            cnt = pr["count"]
            print(f"    [{cnt}个前缀] start={pr['start']}, end={pr['end']}")
    print(f"  XpMin={parsed['xp_min']}, XdMin={parsed['xd_min']}")
    print(f"  threads={config['threads']['worker_threads']}")

    # 检查是否需要自定义数据
    has_custom = any(_is_diy_type(scl, t) for t in types)
    if has_custom:
        print("\n  ⚠ 警告: 旧格式使用了自定义字符集或 Unicode 区间。")
        print("  ⚠ 新配置中 types 已保留，但自定义数据需要手动填入 character_set.custom_values。")

    # 校验 prefix_ranges count
    pr = en.get("prefix_ranges")
    if pr:
        total = sum(x["count"] for x in pr)
        if total != pfx_cnt:
            print(f"\n  ⚠ 警告: prefix_ranges count 总和 ({total}) != 前缀总数 ({pfx_cnt})")


if __name__ == "__main__":
    main()

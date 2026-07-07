"""
PBB 引擎执行模块 — 从 main.py 拆分, 供单机/服务端/客户端三方共用.

run(config, engine_bin, out_dir) → {results, max_xp, max_xd, speed}

config 字典使用 engine 键名 (与 engine.hpp kv[...] 及 config_schema.CONFIG_MAP 一致)。
"""
import os, sys, time, tempfile, subprocess, signal

from config_schema import engine_default, ALL_ENGINE_KEYS


def _runtime_base_dir() -> str:
    """Return the source dir in dev, or PyInstaller's extraction dir in release."""
    return getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))


def _custom_bytes(values):
    """解析 custom_values: 字符串 -> UTF-8 编码, 数字列表 -> 字节数组."""
    if isinstance(values, str):
        return values.encode("utf-8")
    return bytes([int(x) & 0xFF for x in values])


def _build_charset(cs: dict) -> str:
    """根据 character_set 配置构建 charset hex 字符串.

    调用前必须 import pbb_core 并 pbb_core.init_exhanzi().
    """
    import pbb_core
    scl = cs["single_char_length"]
    buf = bytearray()

    for t in cs["types"]:
        if scl == 1:
            if t == 1:   buf.extend(b'0123456789')
            elif t == 2: buf.extend(b'abcdefghijklmnopqrstuvwxyz')
            elif t == 3: buf.extend(b'ABCDEFGHIJKLMNOPQRSTUVWXYZ')
            elif t in (4, 5): buf.extend(_custom_bytes(cs.get("custom_values", [])))

        elif scl == 2:
            if t == 1:   buf.extend(pbb_core.get_xila_bytes())
            elif t == 2: buf.extend(pbb_core.get_XILA_bytes())
            elif t == 3: buf.extend(pbb_core.get_ewen_bytes())
            elif t == 4: buf.extend(pbb_core.get_EWEN_bytes())
            elif t == 5: buf.extend(pbb_core.get_lading_bytes())
            elif t == 6: buf.extend(pbb_core.get_LADING_bytes())
            elif t == 7: buf.extend(_custom_bytes(cs.get("custom_values", [])))
            elif t == 8:
                for r in cs.get("unicode_ranges", []):
                    for cp in range(r["start"], r["end"] + 1):
                        buf.extend(pbb_core.encode_unicode(cp))

        elif scl == 3:
            if t == 1:
                pbb_core.load_hanzi(19968, 40959)
                buf.extend(pbb_core.get_charset_bytes())
                pbb_core.reset_charset()
            elif t == 2: buf.extend(pbb_core.get_hanzi_bytes())
            elif t == 3: buf.extend(pbb_core.get_pingjia_bytes())
            elif t == 4: buf.extend(pbb_core.get_pianjia_bytes())
            elif t == 5: buf.extend(pbb_core.get_mangwen_bytes())
            elif t == 6: buf.extend(pbb_core.get_extended_hanzi_3_bytes())
            elif t == 7: buf.extend(_custom_bytes(cs.get("custom_values", [])))
            elif t == 8:
                for r in cs.get("unicode_ranges", []):
                    for cp in range(r["start"], r["end"] + 1):
                        buf.extend(pbb_core.encode_unicode(cp))

        elif scl == 4:
            if t == 1:   buf.extend(pbb_core.get_extended_hanzi_4_bytes())
            elif t == 2: buf.extend(_custom_bytes(cs.get("custom_values", [])))
            elif t == 3:
                for r in cs.get("unicode_ranges", []):
                    for cp in range(r["start"], r["end"] + 1):
                        buf.extend(pbb_core.encode_unicode(cp))

        else:  # scl >= 5 (5+字节)
            if t == 1:   buf.extend(_custom_bytes(cs.get("custom_values", [])))
            elif t == 2:
                for r in cs.get("unicode_ranges", []):
                    for cp in range(r["start"], r["end"] + 1):
                        buf.extend(pbb_core.encode_unicode(cp))

    return bytes(buf).hex()


def _build_params(config: dict, charset_hex: str, result_file: str = "result.txt") -> str:
    """根据配置 + charset_hex 构建引擎 stdin 参数.

    所有 engine 键名来自 config_schema.CONFIG_MAP，默认值通过 engine_default() 统一获取。
    result_file: 引擎输出文件名 (相对 out/). 分布式多 task 并发时应传入唯一名,
                 避免同目录撞文件 (如 f"result_{task_id}.txt").
    """
    # prefix/suffix CSV
    pfx = config.get("prefixes", "")
    sfx = config.get("suffixes", "")
    if isinstance(pfx, list): pfx = ",".join(pfx)
    if isinstance(sfx, list): sfx = ",".join(sfx)
    pfx = ",".join("" if x.strip() == "+" else x.strip() for x in pfx.split(","))
    sfx = ",".join("" if x.strip() == "+" else x.strip() for x in sfx.split(","))
    if not pfx: pfx = ""
    if not sfx: sfx = ""

    scl = config["scl"]
    charset_len = len(bytes.fromhex(charset_hex)) // scl if scl else 0

    # _dv(key): engine_default() 获取默认值 (CONFIG_MAP 唯一定义)
    def _dv(key: str):
        try:
            return engine_default(key)
        except KeyError:
            return None  # 计算字段无默认值

    params = (
        f"team_name={config['team_name']}\n"
        f"n_threads={config.get('n_threads', _dv('n_threads'))}\n"
        f"scl={scl}\ncharset_len={charset_len}\ncharset_bytes={charset_hex}\n"
        f"prefixes={pfx}\nsuffixes={sfx}\n"
        f"mode={config['mode']}\nvariable_len={config['variable_len']}\n"
        f"range_L={config['range_L']}\nrange_R={config['range_R']}\n"
        f"xp_min={config.get('xp_min', _dv('xp_min'))}\n"
        f"xd_min={config.get('xd_min', _dv('xd_min'))}\n"
        f"collect_mode={config.get('collect_mode', _dv('collect_mode'))}\n"
        f"output_xp={config.get('output_xp', _dv('output_xp'))}\n"
        f"output_log={config.get('output_log', _dv('output_log'))}\n"
        f"output_speed={config.get('output_speed', _dv('output_speed'))}\n"
        f"debug_mode={config.get('debug_mode', _dv('debug_mode'))}\n"
        f"result_file={result_file}\n"
    )
    # A1: seed 传递 (config 有 seed 才传; 缺失则引擎用时间熵, 单机行为不变)
    if config.get("seed") is not None:
        params += f"seed={config['seed']}\n"
    if config.get("collect_mode") == 2:
        params += (
            f"c_eight_v_min={config.get('c_eight_v_min', _dv('c_eight_v_min'))}\n"
            f"c_seven_v_min={config.get('c_seven_v_min', _dv('c_seven_v_min'))}\n"
            f"c_hl_min={config.get('c_hl_min', _dv('c_hl_min'))}\n"
            f"c_hp398_min={config.get('c_hp398_min', _dv('c_hp398_min'))}\n"
        )

    # 自检: 所有生成的 engine key 必须在 ALL_ENGINE_KEYS 中注册
    # (仅 debug_mode 开启时检查，避免影响性能)
    if config.get("debug_mode"):
        for line in params.strip().split("\n"):
            if "=" in line:
                key = line.split("=", 1)[0]
                if key not in ALL_ENGINE_KEYS:
                    print(f"[engine] WARNING: unknown engine key '{key}' — "
                          f"not in CONFIG_MAP or _ENGINE_KEYS_COMPUTED", file=sys.stderr)

    return params


def run(config: dict, engine_bin: str = None, out_dir: str = None,
        result_file: str = "result.txt") -> dict:
    """执行单个 task, 返回 {results, max_xp, max_xd, speed}.

    config 两种模式:
      模式 A: character_set -> 内部调 pbb_core 构建 charset
      模式 B: charset_hex     -> 直接使用 (服务端下发)

    config 可选键:
      seed: 随机种子. None=引擎用时间熵 (单机默认). 指定值=确定性 (分布式可复现).

    out_dir:     输出目录. None=临时目录(用完删除), "."=当前目录保留.
    result_file: 引擎结果文件名 (相对 out/). 分布式多 task 并发同目录时传唯一名.
    """
    # 字符集
    if "charset_hex" in config:
        charset_hex = config["charset_hex"]
    else:
        # pbb_core 在 build/ 目录
        import sys as _sys
        _build_dir = os.path.join(_runtime_base_dir(), "build")
        if _build_dir not in _sys.path:
            _sys.path.insert(0, _build_dir)
        import pbb_core
        pbb_core.init_exhanzi()
        charset_hex = _build_charset(config["character_set"])

    # 线程数
    n_threads = config.get("n_threads", -1)
    if n_threads == -1:
        n_threads = os.cpu_count() or 4
    config["n_threads"] = n_threads

    params = _build_params(config, charset_hex, result_file)

    # 引擎路径
    if engine_bin is None:
        base = _runtime_base_dir()
        engine_bin = os.path.join(base, "build", "pbb_engine")
        if sys.platform == "win32":
            engine_bin += ".exe"

    # 输出目录
    _use_temp = out_dir is None
    if _use_temp:
        _tmp = tempfile.TemporaryDirectory()
        out_dir = _tmp.name

    result_dir = os.path.join(out_dir, "out")
    os.makedirs(result_dir, exist_ok=True)
    cwd = os.getcwd()

    _interrupted = False
    _proc = None

    def _on_sigint(sig, frame):
        nonlocal _interrupted
        if _interrupted:
            # 第二次 Ctrl+C: 强制退出
            os._exit(130)
        _interrupted = True
        print("", file=sys.stderr, flush=True)
        print("[engine] Interrupted — stopping engine...", file=sys.stderr, flush=True)
        if _proc and _proc.poll() is None:
            _proc.terminate()

    old_handler = signal.signal(signal.SIGINT, _on_sigint)

    try:
        os.chdir(out_dir)
        t0 = time.time()
        proc = subprocess.Popen(
            [engine_bin], stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            encoding="utf-8", errors="replace")
        _proc = proc
        proc.stdin.write(params)
        proc.stdin.close()

        # 引擎权威摘要 (问题4/5: SUMMARY 行由引擎输出, Python 采信不重算)
        summary = None
        for line in proc.stderr:
            line = line.strip()
            if not line:
                continue
            if line.startswith("SUMMARY "):
                # 解析 key=value 摘要; 不当普通进度行打印 (避免污染输出)
                summary = {}
                for tok in line[len("SUMMARY "):].split():
                    k, _, v = tok.partition("=")
                    summary[k] = v
                continue
            print(f"  {line}", file=sys.stderr, flush=True)
        proc.wait()
        elapsed = time.time() - t0

        if not _interrupted and proc.returncode != 0:
            raise RuntimeError(f"Engine failed ({proc.returncode})")

        # 读取结果 (名字列表仍从文件读; max/speed 优先采信引擎摘要)
        results = []
        mxp, mxd = 0, 0
        result_path = os.path.join(result_dir, result_file)
        if os.path.exists(result_path):
            with open(result_path, encoding="utf-8", errors="replace") as f:
                for line in f:
                    line = line.strip()
                    if not line: continue
                    parts = line.rsplit(" ", 2)
                    if len(parts) == 3:
                        try:
                            xp, xd = int(parts[1]), int(parts[2])
                            results.append({"name": parts[0], "xp": xp, "xd": xd})
                            mxp = max(mxp, xp)
                            mxd = max(mxd, xd)
                        except ValueError:
                            pass
                    else:
                        results.append({"name": line, "xp": 0, "xd": 0})
    finally:
        signal.signal(signal.SIGINT, old_handler)
        os.chdir(cwd)
        if _use_temp:
            _tmp.cleanup()

    # 问题4/5: 优先采信引擎权威摘要 (max 含通过 V 值/技能检查的名字、speed 为纯算力吞吐)。
    # 文件重算的 max 仅是达标名字的 max (语义不同), 墙钟反算的 speed 含 IPC 噪声。
    # 摘要缺失时 (旧引擎/异常) 回退到文件重算 + 墙钟反算, 保证健壮性。
    if summary is not None:
        max_xp = int(summary.get("max_xp", mxp))
        max_xd = int(summary.get("max_xd", mxd))
        max_sum = int(summary.get("max_sum", 0))
        found = int(summary.get("found", len(results)))
        speed = float(summary.get("speed", 0.0))
        return {"results": results, "max_xp": max_xp, "max_xd": max_xd,
                "max_sum": max_sum, "found": found, "speed": speed}

    # 回退路径 (无摘要)
    rng = config["range_R"] - config["range_L"]
    speed = rng / elapsed if elapsed > 0 and rng > 0 else 0
    return {"results": results, "max_xp": mxp, "max_xd": mxd,
            "max_sum": 0, "found": len(results), "speed": speed}

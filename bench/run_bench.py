# ============================================================================
# run_bench.py — 直接驱动引擎的高优先级基准测试
# 用法: python bench/run_bench.py <engine_exe> <range_end> [result_tag]
# 输出: 一行速度 (M/s), 以及 SUMMARY 的 found/max_xp/max_xd 用于正确性校验
# ============================================================================
import os, sys, subprocess, time, ctypes, signal

def set_high_priority(pid):
    """Windows: 设置进程为 HIGH_PRIORITY_CLASS 减少后台干扰."""
    if sys.platform == "win32":
        try:
            PROCESS_SET_INFORMATION = 0x0200
            HIGH_PRIORITY_CLASS = 0x80
            handle = ctypes.windll.kernel32.OpenProcess(PROCESS_SET_INFORMATION, False, pid)
            if handle:
                ctypes.windll.kernel32.SetPriorityClass(handle, HIGH_PRIORITY_CLASS)
                ctypes.windll.kernel32.CloseHandle(handle)
        except Exception:
            pass

def set_cpu_affinity(pid, cores):
    """Windows: 将进程固定到指定逻辑核心, 减少调度抖动."""
    if sys.platform == "win32":
        try:
            mask = 0
            for c in cores:
                mask |= (1 << c)
            PROCESS_SET_INFORMATION = 0x0200
            handle = ctypes.windll.kernel32.OpenProcess(PROCESS_SET_INFORMATION, False, pid)
            if handle:
                ctypes.windll.kernel32.SetProcessAffinityMask(handle, mask)
                ctypes.windll.kernel32.CloseHandle(handle)
        except Exception:
            pass

def build_params():
    """复刻 engine.py _build_params, 使用 test.config.json 的参数 (mode1, 10 emoji)."""
    team = "test"
    n_threads = 2
    scl = 4
    charset_hex = "f09f9988f09f9989f09f998af09f90b5f09f9092f09f909bf09f908df09f908af09fa68ef09f9089"
    prefixes = "\x01".join(["test-"])
    suffixes = "\x01".join([""])
    return (f"team_name={team}\nn_threads={n_threads}\nscl={scl}\n"
            f"charset_len=10\ncharset_bytes={charset_hex}\n"
            f"prefixes={prefixes}\nsuffixes={suffixes}\n"
            f"mode=1\nvariable_len=8\nrange_L=0\nrange_R={range_end}\n"
            f"xp_min=4900\nxd_min=5600\ncollect_mode=1\n"
            f"output_xp=1\noutput_log=1\noutput_speed=1\ndebug_mode=0\n"
            f"result_file={result_file}\n")

if __name__ == "__main__":
    engine_exe = sys.argv[1]
    range_end = int(sys.argv[2])
    result_file = sys.argv[3] if len(sys.argv) > 3 else "bench_result.txt"

    params = build_params()

    # 清理旧结果
    outdir = os.path.join(os.getcwd(), "out")
    os.makedirs(outdir, exist_ok=True)
    for f in (result_file, "blue.txt"):
        p = os.path.join(outdir, f)
        if os.path.exists(p):
            os.remove(p)

    t0 = time.time()
    proc = subprocess.Popen([engine_exe], stdin=subprocess.PIPE,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            encoding="utf-8", errors="replace")
    set_high_priority(proc.pid)
    # 固定到核心 0/1 (P 核) 与核心 8/9 (P 核) 之一 — 用默认前两个逻辑核心
    affinity_cores = os.environ.get("BENCH_CORES", "0,1")
    set_cpu_affinity(proc.pid, [int(x) for x in affinity_cores.split(",")])
    proc.stdin.write(params)
    proc.stdin.close()

    summary = {}
    for line in proc.stderr:
        line = line.strip()
        if line.startswith("SUMMARY "):
            for tok in line[len("SUMMARY "):].split():
                k, _, v = tok.partition("=")
                summary[k] = v
    proc.wait()
    elapsed = time.time() - t0
    speed = float(summary.get("speed", 0))
    found = summary.get("found", "?")
    max_xp = summary.get("max_xp", "?")
    max_xd = summary.get("max_xd", "?")
    print(f"SPEED={speed/1e6:.4f}M/s FOUND={found} MAX_XP={max_xp} MAX_XD={max_xd} wall={elapsed:.1f}s")

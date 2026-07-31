"""交替 A/B 测试：base vs opt，减少环境噪声影响。
用法: python bench/alt_test.py <base_exe> <opt_exe> <n_pairs> [size]
输出: 每轮 SPEED，最后打印中位数与提升比例。
"""
import os
import subprocess
import sys
import time
import ctypes

def set_high_priority(pid):
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

def build_params(range_end, result_file):
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

def run_engine(exe, size, tag_idx):
    result_file = f"alt_{tag_idx}.txt"
    params = build_params(size, result_file)
    outdir = os.path.join(os.getcwd(), "out")
    os.makedirs(outdir, exist_ok=True)
    for f in (result_file, "blue.txt"):
        p = os.path.join(outdir, f)
        if os.path.exists(p):
            os.remove(p)
    t0 = time.time()
    proc = subprocess.Popen([exe], stdin=subprocess.PIPE,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                            encoding="utf-8", errors="replace")
    set_high_priority(proc.pid)
    set_cpu_affinity(proc.pid, [8, 9])
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
    print(f"SPEED={speed/1e6:.4f}M/s FOUND={found} MAX_XP={max_xp} MAX_XD={max_xd} wall={elapsed:.1f}s", flush=True)
    return speed / 1e6

def median(vals):
    s = sorted(vals)
    n = len(s)
    if n % 2 == 1:
        return s[n // 2]
    return (s[n // 2 - 1] + s[n // 2]) / 2

def main():
    base_exe, opt_exe = sys.argv[1], sys.argv[2]
    n_pairs = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    size = int(sys.argv[4]) if len(sys.argv) > 4 else 100_000_000
    base_vals, opt_vals = [], []
    ratios = []
    idx = 0
    for i in range(n_pairs):
        if i % 2 == 0:
            order = [(base_exe, base_vals, "B"), (opt_exe, opt_vals, "O")]
        else:
            order = [(opt_exe, opt_vals, "O"), (base_exe, base_vals, "B")]
        for exe, vals, tag in order:
            v = run_engine(exe, size, idx)
            idx += 1
            if v is not None:
                vals.append(v)
                print(f"round {i} {tag}: {v:.4f} M/s", flush=True)
        bi, oi = len(base_vals) - 1, len(opt_vals) - 1
        if base_vals[bi] > 0:
            ratios.append(opt_vals[oi] / base_vals[bi])
    bm, om = median(base_vals), median(opt_vals)
    print(f"\nbase samples: {[f'{v:.4f}' for v in base_vals]}")
    print(f"opt  samples: {[f'{v:.4f}' for v in opt_vals]}")
    print(f"base median: {bm:.4f} M/s")
    print(f"opt  median: {om:.4f} M/s")
    print(f"median improvement: {om/bm*100-100:+.2f}%")
    if ratios:
        print(f"paired ratios: {[f'{r:.4f}' for r in ratios]}")
        print(f"paired mean improvement: {(sum(ratios)/len(ratios)-1)*100:+.2f}%")

if __name__ == "__main__":
    main()

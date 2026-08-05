#!/usr/bin/env python3
"""CI 性能回归检查 — 同 runner A/B 交替基准 (消除 runner 机器异构)。

原理: GitHub runner 每次跑在异构机器上, 绝对速度不可比。
方案: 在同一 runner 内编译 merge-base 基线引擎 + PR 引擎,
      用 bench/alt_test.py 交替跑 2 个引擎 (交替顺序抵消时间漂移),
      对比中位数提升。若提升低于阈值 (默认 -5%) 判为回归。

用法: python3 bench/ci_perf_check.py [--pairs N] [--size N] [--threshold PCT]
退出码: 0=通过, 1=回归超阈值
"""
import argparse
import os
import re
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pairs", type=int, default=4, help="A/B 交替轮数")
    ap.add_argument("--size", type=int, default=50_000_000, help="每引擎枚举区间大小")
    ap.add_argument("--threshold", type=float, default=-5.0,
                    help="回归阈值(%%): median improvement 低于此值判失败")
    ap.add_argument("--keep-worktree", action="store_true", help="保留 worktree (调试用)")
    args = ap.parse_args()

    repo = os.getcwd()

    # 1. 获取 merge-base (PR 与 main 的公共祖先 = 性能基线)
    try:
        merge_base = subprocess.check_output(
            ["git", "merge-base", "origin/main", "HEAD"]).decode().strip()
    except subprocess.CalledProcessError:
        print("FAIL: cannot compute merge-base (need full history, fetch-depth: 0)", file=sys.stderr)
        return 1
    print(f"[perf] merge-base: {merge_base}")

    # 2. 用 worktree 编译基线引擎
    worktree = "/tmp/pbb-perf-base"
    subprocess.run(["git", "worktree", "remove", "--force", worktree],
                   capture_output=True)
    if subprocess.run(["git", "worktree", "add", worktree, merge_base],
                      capture_output=True).returncode != 0:
        print("FAIL: git worktree add", file=sys.stderr)
        return 1
    try:
        build_log = subprocess.run(
            ["./run.sh", "-y", "-c", "test.config.json", "--rebuild", "--range-end", "1000000"],
            cwd=worktree, capture_output=True, text=True)
        if build_log.returncode != 0:
            print("FAIL: base engine build failed", file=sys.stderr)
            print(build_log.stderr[-1500:], file=sys.stderr)
            return 1
        base_exe = os.path.join(worktree, "build", "pbb_engine")
        if not os.path.exists(base_exe):
            print("FAIL: base engine not found", file=sys.stderr)
            return 1

        # 3. 编译 PR 引擎 (当前 checkout)
        build_log = subprocess.run(
            ["./run.sh", "-y", "-c", "test.config.json", "--rebuild", "--range-end", "1000000"],
            cwd=repo, capture_output=True, text=True)
        if build_log.returncode != 0:
            print("FAIL: PR engine build failed", file=sys.stderr)
            print(build_log.stderr[-1500:], file=sys.stderr)
            return 1
        opt_exe = os.path.join(repo, "build", "pbb_engine")

        # 4. A/B 交替基准 (同一 runner, 交替顺序抵消漂移)
        # 用 origin/main 的 alt_test.py: PR 分支可能基于旧 main 没有 bench/ 目录
        alt_src = subprocess.check_output(
            ["git", "show", "origin/main:bench/alt_test.py"]).decode()
        alt_path = "/tmp/pbb_alt_test.py"
        with open(alt_path, "w") as f:
            f.write(alt_src)
        alt = subprocess.run(
            [sys.executable, alt_path,
             base_exe, opt_exe, str(args.pairs), str(args.size)],
            capture_output=True, text=True, timeout=1800)
        print(alt.stdout, flush=True)
        if alt.returncode != 0:
            print("FAIL: alt_test failed", file=sys.stderr)
            print(alt.stderr[-1000:], file=sys.stderr)
            return 1

        # 5. 解析 median improvement
        m = re.search(r"median improvement:\s*([+-]?[\d.]+)%", alt.stdout)
        if not m:
            print("FAIL: cannot parse alt_test output", file=sys.stderr)
            return 1
        impr = float(m.group(1))
        print(f"\n[perf] median improvement: {impr:+.2f}% (threshold: {args.threshold}%)")
        if impr < args.threshold:
            print(f"FAIL: performance regression ({impr:+.2f}% < {args.threshold}%)")
            return 1
        print("PASS: no performance regression")
        return 0
    finally:
        if not args.keep_worktree:
            subprocess.run(["git", "worktree", "remove", "--force", worktree],
                           capture_output=True)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Release entry point.

This skips build-time compiler detection because the Windows release bundle
ships with prebuilt binaries for users who do not have Python or g++ installed.
"""
from __future__ import annotations

import os
import sys

import main as app_main


def _bundle_base() -> str:
    return getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))


def _release_engine_path() -> str:
    name = "pbb_engine.exe" if sys.platform == "win32" else "pbb_engine"
    return os.path.join(_bundle_base(), "build", name)


def _ensure_release_engine(rebuild: bool = False, verbose: bool = False) -> str:
    if rebuild:
        print("[release] --rebuild is not available in the prebuilt release.", file=sys.stderr)
    engine_bin = _release_engine_path()
    if not os.path.exists(engine_bin):
        print(f"ERROR: bundled engine not found: {engine_bin}", file=sys.stderr)
        sys.exit(1)
    return engine_bin


def main() -> None:
    app_main.ensure_all = _ensure_release_engine
    app_main.main()


if __name__ == "__main__":
    main()

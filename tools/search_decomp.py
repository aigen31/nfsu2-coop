#!/usr/bin/env python3
"""Search the Ghidra decompile-all output (build/decomp/all.c) for patterns.

Usage: python tools/search_decomp.py "0x554" "PANELVAN" ...
Prints the function header (// name @ addr) and the matching lines with context.
"""
import re
import sys
from pathlib import Path

ALL_C = Path(__file__).resolve().parent.parent / "build" / "decomp" / "all.c"


def main() -> int:
    patterns = sys.argv[1:]
    if not patterns:
        print(__doc__)
        return 2
    if not ALL_C.exists() or ALL_C.stat().st_size == 0:
        print(f"{ALL_C} is missing or empty (is the Ghidra run finished?)")
        return 1
    header = re.compile(r"^// (.+) @ ([0-9a-fA-F]+)$")
    current = "?"
    lines = ALL_C.read_text(encoding="utf-8", errors="replace").splitlines()
    hits = 0
    for i, line in enumerate(lines):
        m = header.match(line)
        if m:
            current = f"{m.group(1)} @ {m.group(2)}"
            continue
        for p in patterns:
            if p in line:
                hits += 1
                print(f"[{current}] {line.strip()}")
                break
    print(f"-- {hits} matching lines")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Compare the market block of two Stellaris saves.

Usage:
    python verify_fix.py A.sav B.sav

A.sav = saved with a standing monthly order
B.sav = loaded from A, then saved immediately without doing anything

Before the patch, resources_bought / internal_market_fluctuations will have
grown by the order amount. After the patch they must be identical.
"""
import sys
import zipfile


def market_block(path):
    with zipfile.ZipFile(path) as z:
        data = z.read("gamestate").decode("utf-8", errors="replace")
    lines = data.split("\n")
    start = None
    for i, line in enumerate(lines):
        if line.strip() == "market=":
            start = i
            break
    if start is None:
        raise SystemExit(f"no market block in {path}")

    depth = 0
    opened = False
    for j in range(start, len(lines)):
        depth += lines[j].count("{") - lines[j].count("}")
        if "{" in lines[j]:
            opened = True
        if opened and depth == 0:
            return lines[start:j + 1]
    raise SystemExit("unterminated market block")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)

    a = market_block(sys.argv[1])
    b = market_block(sys.argv[2])

    print(f"A: {sys.argv[1]}  ({len(a)} lines)")
    print(f"B: {sys.argv[2]}  ({len(b)} lines)")
    print()

    if a == b:
        print("IDENTICAL - the patch is working.")
        return 0

    print("DIFFERENT - the market state changed on load:")
    print()
    import difflib
    for line in difflib.unified_diff(a, b, "A", "B", lineterm="", n=2):
        print(line)
    return 1


if __name__ == "__main__":
    sys.exit(main())

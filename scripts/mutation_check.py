#!/usr/bin/env python3
"""Checks that the differential test actually catches bugs.

Each mutation below is a small, plausible mistake in the engine: an off-by-one in a probe
sequence, a missed aggregate update, a wrong boundary in the fill-or-kill check. The script
applies one at a time, rebuilds, runs the differential suite, and restores the file. A mutation
the suite fails to catch means the fuzzer has a blind spot.

    python3 scripts/mutation_check.py [build-dir]
"""

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build"

MUTATIONS = [
    ("include/lob/flat_order_map.hpp",
     "if (((j - h) & mask_) >= ((j - hole) & mask_)) {",
     "if (((j - h) & mask_) > ((j - hole) & mask_)) {",
     "index: backward-shift deletion off by one"),
    ("include/lob/hierarchical_bitset.hpp",
     "bit(b + 1) - 1;",
     "bit(b) - 1;",
     "bitset: find_prev skips its starting bit"),
    ("include/lob/vector_ladder.hpp",
     "if (!better((it - 1)->price, p)) return it;",
     "if (better(p, (it - 1)->price)) return it;",
     "vector ladder: equal price located on the wrong side"),
    ("include/lob/order_book.hpp",
     "        r.qty -= fill;\n        lvl.total -= fill;",
     "        r.qty -= fill;",
     "book: level total not reduced by a fill"),
    ("include/lob/order_book.hpp",
     "      if (qty > o.qty) {\n        unlink(o, lvl);",
     "      if (qty > o.qty + 1000) {\n        unlink(o, lvl);",
     "book: quantity increase keeps queue priority"),
    ("include/lob/dense_ladder.hpp",
     "    if (i == best_) best_ = next_worse(i);",
     "",
     "dense ladder: best not advanced when best level empties"),
    ("include/lob/order_book.hpp",
     "        if (lvl.total >= needed) {",
     "        if (lvl.total > needed) {",
     "book: FOK rejects an exact fill"),
    ("include/lob/order_book.hpp",
     "          if (config_.stp == StpMode::CancelResting) continue;",
     "          continue;",
     "book: FOK check ignores a self-trade stop"),
]


def run_suite() -> str:
    subprocess.run(["cmake", "--build", str(BUILD)], cwd=ROOT, capture_output=True, check=True)
    try:
        r = subprocess.run([str(BUILD / "tests" / "lob_tests"), "--gtest_filter=Differential*"],
                           cwd=ROOT, capture_output=True, timeout=120)
    except subprocess.TimeoutExpired:
        return "caught (hang)"
    return "caught" if r.returncode != 0 else "MISSED"


def main() -> int:
    missed = 0
    for rel, before, after, name in MUTATIONS:
        path = ROOT / rel
        original = path.read_text()
        if original.count(before) != 1:
            print(f"stale mutation, pattern not found exactly once: {name}")
            return 2
        path.write_text(original.replace(before, after))
        try:
            verdict = run_suite()
        finally:
            path.write_text(original)
        missed += verdict == "MISSED"
        print(f"{verdict:14} {name}", flush=True)
    subprocess.run(["cmake", "--build", str(BUILD)], cwd=ROOT, capture_output=True)
    print(f"\n{len(MUTATIONS) - missed}/{len(MUTATIONS)} mutations caught")
    return 1 if missed else 0


if __name__ == "__main__":
    sys.exit(main())

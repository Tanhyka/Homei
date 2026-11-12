"""Regression Gate script for HomeiEngine (verify_gate.py).

Runs the 6 pytest suites, cross-checks test_progress.md, and validates T28
(optional/OPEN in Stage 0, mandatory in Stage 1 via --require-t28 or [x] T28).
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple


HOMEI_ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = HOMEI_ROOT / "HomeiEngine" / "tests"
TEST_PROGRESS_PATH = HOMEI_ROOT / "test_progress.md"

SUITES = [
    "test_parity.py",
    "test_protocol.py",
    "test_synthetic.py",
    "test_tactical_suite.py",
    "test_piecevalues.py",
    "test_bench.py",
]

T28_LPPN = (
    "lppn1 1B2B26/2S2S2V21T21P2/2B2B2B2B21B21/6b02/3B21P23/9/L24b0b0b0b0/"
    "3t0x11B11x0/8v0 x -/- TKKXBBB/TSSKKXLB 119"
)


@dataclass
class ProgressItem:
    tag: str
    checked: bool
    line: str


def parse_test_progress(md_path: Path) -> Dict[str, ProgressItem]:
    if not md_path.is_file():
        raise FileNotFoundError(f"Missing test_progress.md at {md_path}")
    items: Dict[str, ProgressItem] = {}
    pattern = re.compile(r"^- \[([ xX])\] (Test 0\.\d|Test P\.\d|S\d+|T\d+)\b")
    for raw in md_path.read_text(encoding="utf-8").splitlines():
        m = pattern.match(raw.strip())
        if m:
            items[m.group(2)] = ProgressItem(
                tag=m.group(2),
                checked=(m.group(1).lower() == "x"),
                line=raw.strip(),
            )
    return items


def run_pytest_suite(suite_name: str) -> Tuple[int, str]:
    suite_path = TESTS_DIR / suite_name
    if not suite_path.is_file():
        return 1, f"Missing suite file: {suite_path}"
    cmd = [sys.executable, "-m", "pytest", str(suite_path), "-q", "--tb=short"]
    proc = subprocess.run(
        cmd,
        cwd=str(TESTS_DIR),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    out = (proc.stdout or "") + ("\n" + proc.stderr if proc.stderr else "")
    return proc.returncode, out.strip()


def run_t28_check(movetime_ms: int = 5000) -> Tuple[bool, str]:
    if str(HOMEI_ROOT) not in sys.path:
        sys.path.insert(0, str(HOMEI_ROOT))
    if str(TESTS_DIR) not in sys.path:
        sys.path.insert(0, str(TESTS_DIR))

    from clp_ep_client import CLPEPClient, build_state_from_lppn
    from test_tactical_suite import check_mate_pv_terminal, verify_legal_and_pv

    state, b, turn = build_state_from_lppn(T28_LPPN)
    with CLPEPClient() as client:
        out = client.search_movetime(
            state, movetime_ms=movetime_ms, depth=32, nodes=50000000, multipv=1
        )
        not_str, score_str, pv_len = verify_legal_and_pv(out, b, turn)
        last_info = [i for i in out.infos if i.get("multipv", 1) == 1][-1]
        sc = last_info["score"]
        sc_kind = sc.get("kind")
        sc_val = int(sc.get("value", 0)) if sc_kind == "mate" else None
        pv_ok = check_mate_pv_terminal(out, b, turn, 15)
        passed = (
            not_str == "K*a3<"
            and sc_kind == "mate"
            and sc_val is not None
            and 13 <= sc_val <= 15
            and pv_ok
        )
        detail = (
            f"move={not_str}, score={score_str}, pv_len={pv_len}, "
            f"pv_terminal={pv_ok}, time={out.elapsed_ms:.0f}ms"
        )
        return passed, detail


def main() -> int:
    parser = argparse.ArgumentParser(description="HomeiEngine Regression Gate")
    parser.add_argument(
        "--require-t28",
        action="store_true",
        help="Require T28 (K*a3<, score mate 13..15) to PASS.",
    )
    parser.add_argument(
        "--t28-ms",
        type=int,
        default=5000,
        help="Movetime budget (ms) for T28 verification (default: 5000).",
    )
    args = parser.parse_args()

    print("=== HomeiEngine Regression Gate ===")
    progress = parse_test_progress(TEST_PROGRESS_PATH)

    expected_checked = (
        [f"Test 0.{i}" for i in range(1, 7)]
        + [f"Test P.{i}" for i in range(1, 10)]
        + [f"S{i}" for i in range(1, 9)]
        + [f"T{i:02d}" for i in range(1, 28)]
    )
    missing_or_unchecked = [
        tag for tag in expected_checked if tag not in progress or not progress[tag].checked
    ]
    if missing_or_unchecked:
        print(f"[FAIL] test_progress.md unchecked/missing baseline items: {missing_or_unchecked}")
        return 1

    t28_item: Optional[ProgressItem] = progress.get("T28")
    if t28_item is None:
        print("[FAIL] T28 entry missing from test_progress.md")
        return 1
    t28_is_open_in_md = not t28_item.checked
    require_t28 = args.require_t28 or (not t28_is_open_in_md)

    all_ok = True
    for suite in SUITES:
        print(f"\n--- Running {suite} ---")
        rc, out = run_pytest_suite(suite)
        summary_line = out.splitlines()[-1] if out else "(no output)"
        status = "PASS" if rc == 0 else "FAIL"
        print(f"[{status}] {suite}: {summary_line}")
        if rc != 0:
            print(out)
            all_ok = False

    print("\n--- Checking T28 against test_progress.md ---")
    print(f"test_progress.md T28: checked={t28_item.checked} ({'OPEN' if t28_is_open_in_md else 'CLOSED'})")
    if require_t28:
        t28_ok, t28_detail = run_t28_check(movetime_ms=args.t28_ms)
        print(f"[{'PASS' if t28_ok else 'FAIL'}] T28: {t28_detail}")
        if not t28_ok:
            all_ok = False
    else:
        print("[OPEN] T28 is marked OPEN in test_progress.md (not required in Stage 0).")

    print("\n=== GATE SUMMARY ===")
    if all_ok:
        print("GATE RESULT: PASS (exit 0)")
        return 0
    print("GATE RESULT: FAIL (exit 1)")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())

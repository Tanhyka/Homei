"""Verification and benchmarking tool for Homei_v3.exe.
Runs multi-threaded validation on T23-T28, S1-S8 with Threads=4.
Runs bench 6 1000 comparing Homei_v2-2 vs Homei_v3 (1 vs 4 threads).
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import time

HOMEI_ROOT = Path(r"E:\code5\Homei")
TESTS_DIR = HOMEI_ROOT / "HomeiEngine" / "tests"
RELEASE_DIR = HOMEI_ROOT / "HomeiEngine" / "build" / "Engine realese"

if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

from clp_ep_client import (
    CLPEPClient,
    build_state_from_lppn,
    build_state_from_lppn_and_moves,
)
from positions import SYNTHETIC_POSITIONS
from test_tactical_suite import (
    TACTICAL_POSITIONS,
    check_mate_pv_terminal,
    verify_legal_and_pv,
)
from ref.clp import rules as clp_rules
from ref.clp import lppn as clp_lppn


def test_multithreaded_positions(engine_exe: Path, threads: int = 4) -> dict:
    print(f"\n=======================================================")
    print(f"Testing {engine_exe.name} with Threads = {threads}")
    print(f"=======================================================")
    
    results = {}
    with CLPEPClient(executable=engine_exe) as client:
        # Set Threads
        client.set_option("Threads", threads)
        
        # 1. S1 - S8
        print("\n--- Testing Synthetic Positions (S1 - S8) ---")
        for i, pos in enumerate(SYNTHETIC_POSITIONS):
            tag = f"S{i+1}"
            try:
                if tag == "S8":
                    state, b, turn = build_state_from_lppn_and_moves(pos.lppn, pos.history_moves)
                    out = client.search_movetime(state, movetime_ms=3000, depth=6, nodes=50000000, multipv=1)
                else:
                    state, b, turn = build_state_from_lppn(pos.lppn)
                    out = client.search_movetime(state, movetime_ms=3000, depth=32, nodes=50000000, multipv=1)
                
                not_str, score_str, pv_len = verify_legal_and_pv(out, b, turn)
                
                if tag in ("S1", "S2"):
                    passed = (not_str in pos.expected) and (score_str == "mate 1") and check_mate_pv_terminal(out, b, turn, 1)
                elif tag == "S8":
                    passed = (not_str != "Vi5")
                elif hasattr(pos, "forbidden"):
                    passed = (not_str not in pos.forbidden) and (not_str in pos.expected)
                else:
                    passed = (not_str in pos.expected)
                
                status = "PASS" if passed else "FAIL"
                print(f"[{status}] {tag}: move={not_str}, score={score_str}, time={out.elapsed_ms:.0f}ms")
                results[tag] = {"status": status, "move": not_str, "score": score_str, "time_ms": out.elapsed_ms}
            except Exception as e:
                print(f"[FAIL] {tag}: Exception {e}")
                results[tag] = {"status": "FAIL", "error": str(e)}

        # 2. T23 - T28
        print("\n--- Testing Mate Positions (T23 - T28) ---")
        t28_lppn = (
            "lppn1 1B2B26/2S2S2V21T21P2/2B2B2B2B21B21/6b02/3B21P23/9/L24b0b0b0b0/"
            "3t0x11B11x0/8v0 x -/- TKKXBBB/TSSKKXLB 119"
        )
        for pos in TACTICAL_POSITIONS:
            if pos.tid in ["T23", "T24", "T25", "T26", "T27"]:
                try:
                    state, b, turn = build_state_from_lppn(pos.lppn)
                    out = client.search_movetime(state, movetime_ms=5000, depth=32, nodes=50000000, multipv=1)
                    not_str, score_str, pv_len = verify_legal_and_pv(out, b, turn)
                    pv_ok = check_mate_pv_terminal(out, b, turn, pos.max_mate_ply)
                    if pos.kind == "MATE_UNVERIFIED":
                        status = "PASS" if pv_ok else "SKIP"
                    else:
                        status = "PASS" if pv_ok else "FAIL"
                    print(f"[{status}] {pos.tid}: move={not_str}, score={score_str}, pv_terminal={pv_ok}, time={out.elapsed_ms:.0f}ms")
                    results[pos.tid] = {"status": status, "move": not_str, "score": score_str, "time_ms": out.elapsed_ms}
                except Exception as e:
                    print(f"[FAIL] {pos.tid}: Exception {e}")
                    results[pos.tid] = {"status": "FAIL", "error": str(e)}
        
        # Test T28
        try:
            state, b, turn = build_state_from_lppn(t28_lppn)
            out = client.search_movetime(state, movetime_ms=5000, depth=32, nodes=50000000, multipv=1)
            not_str, score_str, pv_len = verify_legal_and_pv(out, b, turn)
            pv_ok = check_mate_pv_terminal(out, b, turn, 15)
            last_info = [i for i in out.infos if i.get("multipv", 1) == 1][-1]
            sc = last_info["score"]
            sc_val = int(sc.get("value", 0)) if sc.get("kind") == "mate" else 0
            passed = (not_str == "K*a3<") and (sc.get("kind") == "mate") and (13 <= sc_val <= 15) and pv_ok
            status = "PASS" if passed else "FAIL"
            print(f"[{status}] T28: move={not_str}, score={score_str}, pv_terminal={pv_ok}, time={out.elapsed_ms:.0f}ms")
            results["T28"] = {"status": status, "move": not_str, "score": score_str, "time_ms": out.elapsed_ms}
        except Exception as e:
            print(f"[FAIL] T28: Exception {e}")
            results["T28"] = {"status": "FAIL", "error": str(e)}

    return results


def run_bench(engine_exe: Path, threads: int = 1, depth: int = 6, movetime_ms: int = 1000) -> dict:
    print(f"\n=======================================================")
    print(f"Running Bench on {engine_exe.name} (Threads={threads}, Depth={depth}, MoveTime={movetime_ms}ms)")
    print(f"=======================================================")
    
    with CLPEPClient(executable=engine_exe) as client:
        if threads > 1:
            client.set_option("Threads", threads)
        client.send_line(f"bench {depth} {movetime_ms}")
        
        lines = []
        while True:
            line = client.read_line(timeout=20.0)
            if not line:
                break
            lines.append(line.strip())
            if line.startswith("bench done"):
                break
            # Print intermediate position bench output
            if "nodes" in line and ("nps" in line or "time" in line):
                print(f"  {line.strip()}")
        
        last = lines[-1] if lines else ""
        print(f"Summary: {last}")
        # Parse bench done <nodes> nodes <time> ms [nps <nps>]
        parts = last.split()
        res = {"raw": last, "nodes": 0, "time_ms": 0, "nps": 0}
        try:
            if len(parts) >= 6 and parts[0] == "bench" and parts[1] == "done":
                res["nodes"] = int(parts[2])
                res["time_ms"] = int(parts[4])
                if len(parts) >= 8 and parts[6] == "nps":
                    res["nps"] = int(parts[7])
                elif res["time_ms"] > 0:
                    res["nps"] = int(res["nodes"] * 1000 / res["time_ms"])
        except Exception as e:
            print(f"Parse error: {e}")
        return res


if __name__ == "__main__":
    v3_exe = RELEASE_DIR / "Homei_v3.exe"
    v2_2_exe = RELEASE_DIR / "Homei_v2-2.exe"
    
    print(f"Checking binaries:\n  V3: {v3_exe.is_file()}\n  V2-2: {v2_2_exe.is_file()}")
    
    # 1. Multi-threaded test Threads=4 on T23-T28, S1-S8
    res_mt = test_multithreaded_positions(v3_exe, threads=4)
    
    # 2. Bench 6 1000 on V2-2 (1 thread)
    res_v2_2 = run_bench(v2_2_exe, threads=1, depth=6, movetime_ms=1000)
    
    # 3. Bench 6 1000 on V3 (1 thread)
    res_v3_1t = run_bench(v3_exe, threads=1, depth=6, movetime_ms=1000)
    
    # 4. Bench 6 1000 on V3 (4 threads)
    res_v3_4t = run_bench(v3_exe, threads=4, depth=6, movetime_ms=1000)
    
    summary = {
        "multithreaded_tests": res_mt,
        "bench_v2_2_1t": res_v2_2,
        "bench_v3_1t": res_v3_1t,
        "bench_v3_4t": res_v3_4t,
    }
    
    out_json = HOMEI_ROOT / "tools" / "bench_v3_summary.json"
    out_json.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"\nSaved summary to {out_json}")

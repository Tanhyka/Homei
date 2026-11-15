"""Step 1 Verification Suite: Gates 1.0(a), 1.0(b), 1a-1d unit tests, 4 hand-crafted 1b positions,
symmetry on >= 2000 positions, and NPS cost check."""

from __future__ import annotations

import functools
from pathlib import Path
import random
import re
import sys
from typing import Dict, List, Tuple
import pytest

HOMEI_ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = HOMEI_ROOT / "HomeiEngine" / "tests"
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

from positions import TACTICAL_POSITIONS, all_section6_lppns
from ref.clp import board as clp_board
from ref.clp import lppn as clp_lppn
from ref.clp import pieces as clp_pieces
from ref.clp import rules as clp_rules
from tools.safe_proc import SafeEngineClient
from tools.verify_gate import T28_LPPN

V3_EXE = HOMEI_ROOT / "HomeiEngine" / "build" / "Engine realese" / "Homei_v3.exe"
NEW_EXE = HOMEI_ROOT / "HomeiEngine" / "build" / "homei.exe"


@functools.lru_cache(maxsize=1)
def generate_test_lppns(target_count: int = 2050, seed: int = 20261114) -> List[str]:
    rng = random.Random(seed)
    seen = set()
    lppns: List[str] = []

    for s in all_section6_lppns():
        if s not in seen:
            seen.add(s)
            lppns.append(s)
    for tp in TACTICAL_POSITIONS:
        if tp.lppn not in seen:
            seen.add(tp.lppn)
            lppns.append(tp.lppn)
    if T28_LPPN not in seen:
        seen.add(T28_LPPN)
        lppns.append(T28_LPPN)

    while len(lppns) < target_count:
        b = clp_board.Board()
        b.setup_initial()
        turn = clp_pieces.Player.CAM
        plies = rng.randint(1, 50)
        for p in range(plies):
            legal = clp_rules.legal_moves(b, turn)
            if not legal:
                break
            mv = rng.choice(legal)
            clp_rules.apply_move(b, turn, mv)
            turn = clp_rules.other(turn)
            if p >= 1 and len(lppns) < target_count:
                s = clp_lppn.to_lppn(b, turn, ply=p + 1, strict=True)
                if s not in seen:
                    seen.add(s)
                    lppns.append(s)

    return lppns[:target_count]


def query_eval(client: SafeEngineClient, lppn_str: str) -> Tuple[int, int, int, int]:
    client.set_position_lppn(lppn_str)
    client.send_line("eval")
    line = client.read_line()
    assert line.startswith("eval "), f"Unexpected eval output: {line!r}"
    parts = line.split()
    return int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])


def query_evalterms(client: SafeEngineClient, lppn_str: str) -> Tuple[Dict[str, Tuple[int, int, int]], int]:
    client.send_line(f"evalterms {lppn_str}")
    header = client.read_line()
    assert header == "evalterms begin", f"Expected 'evalterms begin', got {header!r}"
    terms: Dict[str, Tuple[int, int, int]] = {}
    total_val: int = 0
    while True:
        line = client.read_line()
        if line == "evalterms end":
            break
        if line.startswith("term "):
            m = re.match(r"^term name=(\S+) raw=(-?\d+) weight=(-?\d+) contrib=(-?\d+)$", line)
            assert m is not None, f"Malformed term line: {line!r}"
            name = m.group(1)
            raw_v = int(m.group(2))
            w_v = int(m.group(3))
            c_v = int(m.group(4))
            assert raw_v * w_v == c_v, f"Term {name}: {raw_v} * {w_v} != {c_v}"
            terms[name] = (raw_v, w_v, c_v)
        elif line.startswith("total "):
            total_val = int(line.split()[1])
    return terms, total_val


def run_bench_nodes(client: SafeEngineClient, depth: int = 6, movetime_ms: int = 30000) -> Tuple[int, int, List[str]]:
    client.send_line(f"bench {depth} {movetime_ms}")
    rows: List[str] = []
    total_nodes = 0
    total_ms = 0
    while True:
        line = client.read_line(timeout=60.0)
        if line.startswith("bench done "):
            parts = line.split()
            total_nodes = int(parts[2])
            total_ms = int(parts[4])
            break
        rows.append(line)
    return total_nodes, total_ms, rows


def test_gate_1_0_a_and_b_bit_identical_and_evalterms():
    """Gate 1.0(a) & 1.0(b):
    - With koma_eval=0 and no HOMEI_PARAMS, eval() matches Homei_v3.exe on >= 2000 positions,
      and fixed-depth bench node count matches Homei_v3.exe 100%.
    - Sum of evalterms contributions matches eval() 100% on >= 2000 positions.
    """
    lppns = generate_test_lppns(2050)
    assert len(lppns) >= 2000

    # Use -mate_root and 30000ms per position so bench is purely node-deterministic (no wall-clock cutoffs)
    det_env = {"HOMEI_FEATURES": "-mate_root,-koma_eval,-koma_search", "HOMEI_PARAMS": ""}
    with SafeEngineClient(V3_EXE, extra_env=det_env, cmd_timeout=60.0) as c_v3, SafeEngineClient(
        NEW_EXE, extra_env=det_env, cmd_timeout=60.0
    ) as c_new:
        nodes_v3, _, rows_v3 = run_bench_nodes(c_v3, depth=6, movetime_ms=30000)
        nodes_new, _, rows_new = run_bench_nodes(c_new, depth=6, movetime_ms=30000)
        assert nodes_new == nodes_v3, f"Bench depth 6 nodes mismatch: new={nodes_new} != v3={nodes_v3}"

        for idx, lp in enumerate(lppns):
            ev_v3 = query_eval(c_v3, lp)
            ev_new = query_eval(c_new, lp)
            assert ev_new == ev_v3, f"Eval mismatch at pos {idx} ({lp}): new={ev_new} != v3={ev_v3}"

            terms, tot = query_evalterms(c_new, lp)
            contrib_sum = sum(v[2] for v in terms.values())
            assert contrib_sum == tot == ev_new[0], (
                f"evalterms sum mismatch at pos {idx}: sum={contrib_sum}, total={tot}, eval={ev_new[0]}"
            )


def test_step_1b_four_handcrafted_positions():
    """Verify the 4 hand-crafted test cases for 1b (Buried slider and battery) + symmetry."""
    # Case (1): Tướng/Sĩ burying Long/Phượng/Xa in home territory (rel_row <= 3 for CAM)
    pos1 = "lppn1 4V24/4[L2T2]4/9/9/9/9/9/9/4v04 c -/- -/- 10"

    # Case (2): Sĩ stacked on Phượng [P2S2] in enemy half (f7 -> r=5, col=2, rel_row=5 >= 4)
    # with clear diagonal ray f7(5,2) -> g6(6,3) -> h5(7,4) into 3x3 around XANH King v0 at i5(8,4).
    pos2 = "lppn1 4V24/9/9/9/9/2[P2S2]6/9/9/4v04 c -/- -/- 20"

    # Case (3): Same as Case (2), but ray is blocked by a piece b0 at g6 (r=6, col=3).
    pos3 = "lppn1 4V24/9/9/9/9/2[P2S2]6/3b05/9/4v04 c -/- -/- 20"

    with SafeEngineClient(
        NEW_EXE, extra_env={"HOMEI_FEATURES": "+koma_eval,-koma_search", "HOMEI_PARAMS": ""}
    ) as client:
        # Case (1): Home-buried slider -> penalized (-45 or tuned -47), NO +20 battery
        t1, tot1 = query_evalterms(client, pos1)
        assert t1["koma_buried_home"][0] == -1 and t1["koma_buried_home"][2] == -t1["koma_buried_home"][1] < -20
        assert t1["stack_battery"][0] == 0 and t1["stack_battery"][2] == 0

        # Case (2): Enemy half (rel_row >= 4) with clear siege ray into 3x3 enemy King -> 0 penalty, +20 battery
        t2, tot2 = query_evalterms(client, pos2)
        assert t2["koma_buried_home"][0] == 0
        assert t2["koma_buried_no_ray"][0] == 0
        assert t2["stack_battery"][0] == 1 and t2["stack_battery"][2] == 20

        # Case (3): Same as (2) but ray blocked -> penalized (-30), NO +20 battery
        t3, tot3 = query_evalterms(client, pos3)
        assert t3["koma_buried_no_ray"][0] == -1 and t3["koma_buried_no_ray"][2] == -30
        assert t3["stack_battery"][0] == 0 and t3["stack_battery"][2] == 0
        assert tot2 > tot3

        # Case (4): Horizontal mirror and 180-degree color flip of (1), (2), (3) give exact symmetric scores
        for lp in (pos1, pos2, pos3):
            ev, ev_mir, ev_cam, ev_flp_cam = query_eval(client, lp)
            assert ev == ev_mir, f"Mirror symmetry failed on {lp}: {ev} != {ev_mir}"
            assert ev_cam == -ev_flp_cam, f"180-flip symmetry failed on {lp}: {ev_cam} != -{ev_flp_cam}"


def test_koma_eval_symmetry_2000_positions_and_nps():
    """Verify 100% symmetry on >= 2000 positions with koma_eval=1 and check NPS cost <= 10%."""
    lppns = generate_test_lppns(2050)
    with SafeEngineClient(
        NEW_EXE, extra_env={"HOMEI_FEATURES": "+koma_eval,-koma_search", "HOMEI_PARAMS": ""}
    ) as client_on, SafeEngineClient(
        NEW_EXE, extra_env={"HOMEI_FEATURES": "-koma_eval,-koma_search", "HOMEI_PARAMS": ""}
    ) as client_off:
        for idx, lp in enumerate(lppns):
            ev, ev_mir, ev_cam, ev_flp_cam = query_eval(client_on, lp)
            assert ev == ev_mir, f"Mirror symmetry failed at {idx} ({lp}): {ev} != {ev_mir}"
            assert ev_cam == -ev_flp_cam, f"180-flip symmetry failed at {idx} ({lp}): {ev_cam} != -{ev_flp_cam}"

        for idx in range(200):
            terms, tot = query_evalterms(client_on, lppns[idx])
            ev, _, _, _ = query_eval(client_on, lppns[idx])
            assert sum(v[2] for v in terms.values()) == tot == ev

        # Measure NPS on bench depth 6 (1000ms), best of 2 runs to smooth OS jitter
        best_nps_off = 0.0
        best_nps_on = 0.0
        for _ in range(2):
            n_off, ms_off, _ = run_bench_nodes(client_off, depth=6, movetime_ms=1000)
            n_on, ms_on, _ = run_bench_nodes(client_on, depth=6, movetime_ms=1000)
            best_nps_off = max(best_nps_off, (n_off * 1000.0) / max(1, ms_off))
            best_nps_on = max(best_nps_on, (n_on * 1000.0) / max(1, ms_on))
        slowdown_pct = 100.0 * (best_nps_off - best_nps_on) / max(1.0, best_nps_off)
        print(f"\n[NPS check] OFF={best_nps_off:.0f} nps, ON={best_nps_on:.0f} nps, slowdown={slowdown_pct:+.2f}%")
        assert slowdown_pct <= 10.0, f"NPS slowdown {slowdown_pct:.2f}% exceeds 10% limit!"

"""Measure stop -> bestmove latency across 200 positions (T01-T28 + random positions)."""

from __future__ import annotations

import json
from pathlib import Path
import random
import statistics
import sys
import time
from typing import List

HOMEI_ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = HOMEI_ROOT / "HomeiEngine" / "tests"
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

from positions import TACTICAL_POSITIONS
from ref.clp import board as clp_board
from ref.clp import lppn as clp_lppn
from ref.clp import pieces as clp_pieces
from ref.clp import rules as clp_rules
from tools.safe_proc import SafeEngineClient
from tools.verify_gate import T28_LPPN


DEFAULT_ENGINE = HOMEI_ROOT / "HomeiEngine" / "build" / "homei.exe"


def collect_200_positions(seed: int = 12345) -> List[str]:
    rng = random.Random(seed)
    lppns: List[str] = []
    seen = set()

    for tp in TACTICAL_POSITIONS:
        if tp.lppn not in seen:
            seen.add(tp.lppn)
            lppns.append(tp.lppn)

    if T28_LPPN not in seen:
        seen.add(T28_LPPN)
        lppns.append(T28_LPPN)

    while len(lppns) < 200:
        b = clp_board.Board()
        b.setup_initial()
        turn = clp_pieces.Player.CAM
        target_plies = rng.randint(6, 36)
        ok = True
        for p in range(target_plies):
            legal = clp_rules.legal_moves(b, turn)
            if not legal:
                ok = False
                break
            mv = rng.choice(legal)
            clp_rules.apply_move(b, turn, mv)
            turn = clp_rules.other(turn)
        if not ok:
            continue
        if not clp_rules.legal_moves(b, turn):
            continue
        s = clp_lppn.to_lppn(b, turn, ply=target_plies, strict=True)
        if s not in seen:
            seen.add(s)
            lppns.append(s)

    return lppns[:200]


def run_stop_latency_test(engine_path: Path = DEFAULT_ENGINE, seed: int = 12345) -> dict:
    positions = collect_200_positions(seed=seed)
    rng = random.Random(seed + 99)
    latencies_ms: List[float] = []
    over_200ms: List[dict] = []

    with SafeEngineClient(engine_path) as client:
        for idx, lppn_str in enumerate(positions):
            client.set_position_lppn(lppn_str)
            client._next_id += 1
            req_id = client._next_id
            delay_s = rng.uniform(0.020, 0.250)

            client.send_line(f"go id {req_id} movetime 2000")
            time.sleep(delay_s)
            t_stop = time.perf_counter()
            client.send_line(f"stop id {req_id}")

            while True:
                line = client.read_line(timeout=5.0)
                if line.startswith(f"bestmove id {req_id} "):
                    t_bm = time.perf_counter()
                    break

            lat_ms = max(0.0, (t_bm - t_stop) * 1000.0)
            latencies_ms.append(lat_ms)
            if lat_ms > 200.0:
                over_200ms.append({"index": idx, "lppn": lppn_str, "latency_ms": round(lat_ms, 2)})

    sorted_lat = sorted(latencies_ms)
    n = len(sorted_lat)
    summary = {
        "engine": str(engine_path),
        "positions": n,
        "min_ms": round(sorted_lat[0], 3),
        "median_ms": round(statistics.median(sorted_lat), 3),
        "mean_ms": round(statistics.mean(sorted_lat), 3),
        "p95_ms": round(sorted_lat[int(0.95 * (n - 1))], 3),
        "p99_ms": round(sorted_lat[int(0.99 * (n - 1))], 3),
        "max_ms": round(sorted_lat[-1], 3),
        "over_200ms_count": len(over_200ms),
        "over_200ms_pct": round(100.0 * len(over_200ms) / n, 2),
        "passed": len(over_200ms) <= 2,
        "over_200ms_details": over_200ms,
    }
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return summary


if __name__ == "__main__":
    eng = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ENGINE
    res = run_stop_latency_test(eng)
    raise SystemExit(0 if res["passed"] else 1)

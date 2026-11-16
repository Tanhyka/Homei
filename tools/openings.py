"""Generate balanced, deduplicated 8-ply openings for Homei strength matches."""

from __future__ import annotations

import argparse
import json
import random
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

HOMEI_ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = HOMEI_ROOT / "HomeiEngine" / "tests"
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

from clp_ep_client import build_state_from_lppn_and_moves
from ref.clp import board as clp_board
from ref.clp import core_bridge
from ref.clp import lppn as clp_lppn
from ref.clp import pieces as clp_pieces
from ref.clp import rules as clp_rules
from tools.safe_proc import SafeEngineClient


DEFAULT_ENGINE = HOMEI_ROOT / "HomeiEngine" / "build" / "Engine realese" / "Homei_v3.exe"
DEFAULT_OUT = HOMEI_ROOT / "openings.txt"


def eval_depth(client: SafeEngineClient, state: dict, depth: int = 4) -> Tuple[str, Optional[int]]:
    """Evaluate a position at fixed depth using SafeEngineClient. Returns (kind, value)."""
    out = client.search_fixed_depth(state, depth=depth, multipv=1, movetime_ms=10000)
    infos = [i for i in out.infos if i.get("multipv", 1) == 1]
    if not infos:
        return "unknown", None
    sc = infos[-1].get("score", {})
    return str(sc.get("kind", "unknown")), int(sc.get("value", 0))


def generate_openings(
    count: int = 300,
    plies: int = 8,
    seed: int = 12345,
    max_abs_cp: int = 150,
    eval_depth_n: int = 4,
    engine_path: Path = DEFAULT_ENGINE,
    out_path: Path = DEFAULT_OUT,
    exclude_path: Optional[Path] = None,
) -> List[Tuple[str, List[str], int]]:
    rng = random.Random(seed)

    base_b = clp_board.Board()
    base_b.setup_initial()
    start_lppn = clp_lppn.to_lppn(base_b, clp_pieces.Player.CAM, ply=0, strict=True)

    seen_lppn: set[str] = set()
    if exclude_path and exclude_path.is_file():
        for raw in exclude_path.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            lppn_str = line.split("\t", 1)[0].strip()
            if lppn_str:
                seen_lppn.add(lppn_str)
        print(f"[openings] Loaded {len(seen_lppn)} excluded LPPNs from {exclude_path}")
    accepted: List[Tuple[str, List[str], int]] = []
    attempts = 0
    rejected_terminal = 0
    rejected_dup = 0
    rejected_score = 0

    t0 = time.perf_counter()
    with SafeEngineClient(engine_path) as client:
        while len(accepted) < count:
            attempts += 1
            b = clp_board.Board()
            b.setup_initial()
            turn = clp_pieces.Player.CAM
            moves_not: List[str] = []

            pos0 = core_bridge._encode_position_py(b, turn)
            seen_pos_counts: Dict[str, int] = {core_bridge._position_identity_py(pos0): 1}
            failed = False

            for _ in range(plies):
                legal = clp_rules.legal_moves(b, turn)
                if not legal:
                    failed = True
                    break
                mv = rng.choice(legal)
                mv_not = mv.canonical_notation(b)
                clp_rules.apply_move(b, turn, mv)
                turn = clp_rules.other(turn)
                moves_not.append(mv_not)

                pos_dict = core_bridge._encode_position_py(b, turn)
                pid = core_bridge._position_identity_py(pos_dict)
                seen_pos_counts[pid] = seen_pos_counts.get(pid, 0) + 1
                if seen_pos_counts[pid] >= 3:
                    failed = True
                    break

            if failed:
                rejected_terminal += 1
                continue

            # Ensure non-terminal at final ply
            state, final_b, final_turn = build_state_from_lppn_and_moves(start_lppn, moves_not)
            if state.get("result", {}).get("status") != "ongoing":
                rejected_terminal += 1
                continue

            canon_lppn = clp_lppn.to_lppn(final_b, final_turn, ply=plies, strict=True)
            if canon_lppn in seen_lppn:
                rejected_dup += 1
                continue

            kind, val = eval_depth(client, state, depth=eval_depth_n)
            if kind != "cp" or val is None or abs(val) > max_abs_cp:
                rejected_score += 1
                continue

            seen_lppn.add(canon_lppn)
            accepted.append((canon_lppn, moves_not, val))
            if len(accepted) % 50 == 0:
                dt = time.perf_counter() - t0
                print(
                    f"[openings] {len(accepted)}/{count} accepted "
                    f"(attempts={attempts}, rej_term={rejected_terminal}, "
                    f"rej_dup={rejected_dup}, rej_score={rejected_score}, {dt:.1f}s)"
                )

    lines = [f"{lppn}\t{' '.join(moves)}" for lppn, moves, _ in accepted]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    dt = time.perf_counter() - t0
    print(
        f"[openings] Saved {len(accepted)} openings to {out_path} in {dt:.1f}s "
        f"(attempts={attempts}, rej_term={rejected_terminal}, "
        f"rej_dup={rejected_dup}, rej_score={rejected_score})"
    )
    return accepted


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate 300 balanced 8-ply openings for Homei.")
    parser.add_argument("--count", type=int, default=300)
    parser.add_argument("--plies", type=int, default=8)
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--max-abs-cp", type=int, default=150)
    parser.add_argument("--depth", type=int, default=4)
    parser.add_argument("--engine", type=Path, default=DEFAULT_ENGINE)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--exclude", type=Path, default=None)
    args = parser.parse_args()

    generate_openings(
        count=args.count,
        plies=args.plies,
        seed=args.seed,
        max_abs_cp=args.max_abs_cp,
        eval_depth_n=args.depth,
        engine_path=args.engine,
        out_path=args.out,
        exclude_path=args.exclude,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

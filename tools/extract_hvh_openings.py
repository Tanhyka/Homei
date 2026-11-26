"""Extract deduplicated + horizontally mirrored HvH human openings (ply 8, 12, 16).

Reads clean LPGF games from E:\\all-code\\Code1\\CLP_dataset\\LPGF\\HvH (skipping
HISTORICAL_ILLEGAL_LOSS and unparseable files), verifies legal replay from
STARTPOS, adds horizontal-mirror counterparts, deduplicates by board/reserve/
prisoners key, optionally filters heavily one-sided positions via engine depth-4
eval, and writes data/openings_hvh_clean.txt in match.py's `<lppn>\\t<moves>` format.
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import os
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

from clp_ep_client import build_state_from_lppn
from ref.clp import board as clp_board
from ref.clp import lppn as clp_lppn
from ref.clp import pieces as clp_pieces
from ref.clp import record_io
from ref.clp import rules as clp_rules
from tools.extract_koma_data import _board_key, _mirror_board_and_turn, _mirror_move
from tools.safe_proc import SafeEngineClient

HVH_DIR = Path(r"E:\all-code\Code1\CLP_dataset\LPGF\HvH")
DEFAULT_OUT = HOMEI_ROOT / "data" / "openings_hvh_clean.txt"
DEFAULT_ENGINE = HOMEI_ROOT / "HomeiEngine" / "build" / "homei.exe"


def _startpos_lppn() -> str:
    b = clp_board.Board()
    b.setup_initial()
    return clp_lppn.to_lppn(b, clp_pieces.Player.CAM, ply=0, strict=True)


STARTPOS_LPPN = _startpos_lppn()


def _replay_and_mirror(
    full_moves: List[str],
    target_plies: Tuple[int, ...],
    include_mirror: bool = True,
) -> Dict[int, Tuple[str, List[str], Optional[str], Optional[List[str]]]]:
    """Single-pass replay of a game producing (lppn, moves, mir_lppn, mir_moves) at each target ply."""
    max_p = max(target_plies)
    want = set(target_plies)
    b = clp_board.Board()
    b.setup_initial()
    turn = clp_pieces.Player.CAM
    mb = _mirror_board_and_turn(b, turn) if include_mirror else None

    orig_moves: List[str] = []
    mir_moves: List[str] = []
    out: Dict[int, Tuple[str, List[str], Optional[str], Optional[List[str]]]] = {}

    for step, mstr in enumerate(full_moves[:max_p], 1):
        legal = clp_rules.legal_moves(b, turn)
        matched = [m for m in legal if m.canonical_notation(b) == mstr]
        if len(matched) != 1:
            break
        mv = matched[0]
        orig_moves.append(mstr)
        clp_rules.apply_move(b, turn, mv)

        if include_mirror and mb is not None:
            mmv = _mirror_move(mv, mb)
            mlegal = clp_rules.legal_moves(mb, turn)
            mmatched = [
                cm for cm in mlegal
                if cm.kind == mmv.kind
                and cm.src == mmv.src
                and cm.dst == mmv.dst
                and cm.ptype == mmv.ptype
                and cm.facing == mmv.facing
                and cm.rotate_after == mmv.rotate_after
            ]
            if len(mmatched) != 1:
                mb = None
            else:
                mir_moves.append(mmatched[0].canonical_notation(mb))
                clp_rules.apply_move(mb, turn, mmatched[0])

        turn = clp_rules.other(turn)
        if step in want:
            lp = clp_lppn.to_lppn(b, turn, ply=step, strict=True)
            mlp = clp_lppn.to_lppn(mb, turn, ply=step, strict=True) if (include_mirror and mb is not None) else None
            out[step] = (
                lp,
                list(orig_moves),
                mlp,
                list(mir_moves) if mlp is not None else None,
            )
    return out


def _eval_depth(client: SafeEngineClient, state: dict, depth: int = 3) -> Tuple[str, Optional[int]]:
    out = client.search_fixed_depth(state, depth=depth, multipv=1, movetime_ms=5000)
    infos = [i for i in out.infos if i.get("multipv", 1) == 1]
    if not infos:
        return "unknown", None
    sc = infos[-1].get("score", {})
    return str(sc.get("kind", "unknown")), int(sc.get("value", 0))


def _process_single_lpgf(args_tuple: Tuple[str, Tuple[int, ...], bool]) -> dict:
    from ref.clp import core_bridge

    fp_str, plies, include_mirror = args_tuple
    fp = Path(fp_str)
    txt = fp.read_text(encoding="utf-8", errors="ignore")
    if "HISTORICAL_ILLEGAL_LOSS" in txt:
        return {"status": "skipped_illegal_loss"}
    rec = record_io.ImportedRecord()
    if not record_io.RecordIO.read(fp_str, rec):
        return {"status": "skipped_read_fail"}
    br = core_bridge.CoreBridge()
    app, _, _ = record_io.RecordIO.load_txt_moves_into_bridge(br, rec)
    if app <= 0:
        return {"status": "skipped_load_fail"}
    nodes = br._active_line_nodes()
    full_moves = [nd.move["notation"] for nd in nodes[1:] if nd.move and "notation" in nd.move]
    snaps = _replay_and_mirror(full_moves, plies, include_mirror=include_mirror)
    entries = []
    skipped_term = 0
    for target_ply in plies:
        if target_ply not in snaps:
            continue
        canon_lp, prefix, mir_lp, mir_prefix = snaps[target_ply]
        st, _, _ = build_state_from_lppn(canon_lp)
        if st.get("result", {}).get("status") != "ongoing":
            skipped_term += 1
            continue
        entries.append((target_ply, canon_lp, prefix, fp.name, mir_lp, mir_prefix))
    return {
        "status": "ok",
        "short": len(full_moves) < min(plies),
        "skipped_terminal": skipped_term,
        "entries": entries,
    }


def extract_hvh_openings(
    hvh_dir: Path = HVH_DIR,
    plies: Tuple[int, ...] = (8, 12, 16),
    max_abs_cp: int = 300,
    eval_depth_n: int = 3,
    engine_path: Path = DEFAULT_ENGINE,
    out_path: Path = DEFAULT_OUT,
    seed: int = 20261118,
    include_mirror: bool = True,
) -> dict:
    t0 = time.perf_counter()
    files = sorted(hvh_dir.glob("*.lpgf"))
    stats = collections.Counter()
    seen_keys: set[str] = set()
    candidates: List[Tuple[int, str, List[str], str, int]] = []  # (ply, lppn, moves, src_name, is_mirror)

    max_workers = max(1, min(6, (os.cpu_count() or 4) - 1))
    tasks = [(str(fp), plies, include_mirror) for fp in files]
    with concurrent.futures.ProcessPoolExecutor(max_workers=max_workers) as pool:
        for res in pool.map(_process_single_lpgf, tasks, chunksize=8):
            st_code = res["status"]
            if st_code != "ok":
                stats[st_code] += 1
                continue
            if res["short"]:
                stats["clean_games_short"] += 1
            stats["clean_games"] += 1
            stats["skipped_terminal"] += res["skipped_terminal"]
            for target_ply, canon_lp, prefix, fname, mir_lp, mir_prefix in res["entries"]:
                bkey = _board_key(canon_lp)
                if bkey not in seen_keys:
                    seen_keys.add(bkey)
                    candidates.append((target_ply, canon_lp, prefix, fname, 0))
                    stats[f"raw_ply_{target_ply}"] += 1
                if mir_lp is not None and mir_prefix is not None:
                    mir_key = _board_key(mir_lp)
                    if mir_key not in seen_keys:
                        seen_keys.add(mir_key)
                        candidates.append((target_ply, mir_lp, mir_prefix, fname, 1))
                        stats[f"mirror_ply_{target_ply}"] += 1

    print(
        f"[hvh-openings] Parsed {stats['clean_games']}/{len(files)} clean games -> {len(candidates)} unique candidates in {time.perf_counter() - t0:.1f}s",
        flush=True,
    )

    accepted: List[Tuple[int, str, List[str], Optional[int]]] = []
    if max_abs_cp > 0 and engine_path.is_file():
        print(
            f"[hvh-openings] Evaluating {len(candidates)} unique candidates at depth={eval_depth_n} "
            f"(|cp| <= {max_abs_cp}) using {engine_path.name}...",
            flush=True,
        )
        with SafeEngineClient(engine_path) as client:
            for idx, (target_ply, lp, mvs, _, _) in enumerate(candidates, 1):
                st, _, _ = build_state_from_lppn(lp)
                kind, val = _eval_depth(client, st, depth=eval_depth_n)
                if kind == "cp" and val is not None and abs(val) <= max_abs_cp:
                    accepted.append((target_ply, lp, mvs, val))
                    stats[f"accepted_ply_{target_ply}"] += 1
                else:
                    stats["rejected_unbalanced"] += 1
                if idx % 200 == 0 or idx == len(candidates):
                    print(
                        f"  checked {idx}/{len(candidates)} -> kept={len(accepted)} "
                        f"({time.perf_counter() - t0:.1f}s)",
                        flush=True,
                    )
    else:
        for target_ply, lp, mvs, _, _ in candidates:
            accepted.append((target_ply, lp, mvs, None))
            stats[f"accepted_ply_{target_ply}"] += 1

    # Interleave plies evenly via deterministic shuffle so short/early test slices cover ply 8, 12, 16 alike
    rng = random.Random(seed)
    rng.shuffle(accepted)

    lines = [f"{lp}\t{' '.join(mvs)}" for _, lp, mvs, _ in accepted]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")

    dt = time.perf_counter() - t0
    summary = {
        "total_files": len(files),
        "clean_games": stats["clean_games"],
        "unique_candidates": len(candidates),
        "accepted_openings": len(accepted),
        "by_ply_accepted": {p: stats[f"accepted_ply_{p}"] for p in plies},
        "stats": dict(stats),
        "out_path": str(out_path),
        "elapsed_s": round(dt, 2),
    }
    print(f"[hvh-openings] Done in {dt:.1f}s -> {out_path}")
    print(f"  Clean games: {summary['clean_games']}/{summary['total_files']} | Unique candidates: {len(candidates)} | Accepted: {len(accepted)}")
    print(f"  By ply accepted: {summary['by_ply_accepted']}")
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description="Extract clean HvH ply 8/12/16 openings with horizontal mirrors.")
    parser.add_argument("--hvh-dir", type=Path, default=HVH_DIR)
    parser.add_argument("--plies", default="8,12,16", help="Comma-separated plies (default: 8,12,16)")
    parser.add_argument("--max-abs-cp", type=int, default=300, help="Filter |eval| > threshold (0 disables)")
    parser.add_argument("--depth", type=int, default=3, help="Fixed eval depth for balance filter")
    parser.add_argument("--engine", type=Path, default=DEFAULT_ENGINE)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--no-mirror", action="store_true")
    parser.add_argument("--seed", type=int, default=20261118)
    args = parser.parse_args()

    plies = tuple(int(x.strip()) for x in args.plies.split(",") if x.strip())
    extract_hvh_openings(
        hvh_dir=args.hvh_dir,
        plies=plies,
        max_abs_cp=args.max_abs_cp,
        eval_depth_n=args.depth,
        engine_path=args.engine,
        out_path=args.out,
        seed=args.seed,
        include_mirror=not args.no_mirror,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

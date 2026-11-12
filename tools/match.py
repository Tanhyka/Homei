"""Headless CLP-EP v1 match runner for Homei engines using ref.clp as referee."""

from __future__ import annotations

import argparse
import concurrent.futures
import copy
import json
import math
import os
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

HOMEI_ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = HOMEI_ROOT / "HomeiEngine" / "tests"
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

from clp_ep_client import (
    CLPEPClient,
    b64url_decode,
    build_state_from_lppn,
    build_state_from_lppn_and_moves,
    encode_state64,
)
from ref.clp import board as clp_board
from ref.clp import core_bridge
from ref.clp import lppn as clp_lppn
from ref.clp import pieces as clp_pieces
from ref.clp import rules as clp_rules


def _startpos_lppn() -> str:
    b = clp_board.Board()
    b.setup_initial()
    return clp_lppn.to_lppn(b, clp_pieces.Player.CAM, ply=0, strict=True)


STARTPOS_LPPN = _startpos_lppn()


def load_openings(path: Path) -> List[Tuple[str, List[str]]]:
    if not path.is_file():
        raise FileNotFoundError(f"Openings file not found: {path}")
    openings: List[Tuple[str, List[str]]] = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "\t" in line:
            lppn_str, moves_str = line.split("\t", 1)
            moves = [m for m in moves_str.strip().split() if m]
        else:
            lppn_str = line
            moves = []
        openings.append((lppn_str.strip(), moves))
    if not openings:
        raise ValueError(f"No openings loaded from {path}")
    return openings


def _configure_engine(client: CLPEPClient) -> None:
    lines = set(client.handshake_lines)
    if any(l.startswith("option name Hash ") for l in lines):
        client.set_option("Hash", 64)
    if any(l.startswith("option name Threads ") for l in lines):
        client.set_option("Threads", 1)
    if any(l.startswith("option name Depth ") for l in lines):
        client.set_option("Depth", 32)
    if any(l.startswith("option name Nodes ") for l in lines):
        client.set_option("Nodes", 50000000)
    if any(l.startswith("option name MultiPV ") for l in lines):
        client.set_option("MultiPV", 1)


def _search_with_deadline(
    client: CLPEPClient,
    state: dict,
    movetime_ms: int,
    grace_ms: int = 1000,
) -> Tuple[str, Optional[dict], List[dict], float]:
    """Send position+go and wait up to movetime_ms + grace_ms.

    Returns (status, bestmove_obj, infos, elapsed_ms) where status is
    'ok' | 'timeout' | 'engine_error'.
    """
    client.set_position_state(state)
    client._next_id += 1
    req_id = client._next_id
    t0 = time.perf_counter()
    deadline = t0 + (movetime_ms + grace_ms) / 1000.0
    client.send_line(f"go id {req_id} movetime {movetime_ms}")

    infos: List[dict] = []
    while True:
        rem = deadline - time.perf_counter()
        if rem <= 0.0:
            return "timeout", None, infos, (time.perf_counter() - t0) * 1000.0
        try:
            line = client.read_line(timeout=rem)
        except TimeoutError:
            return "timeout", None, infos, (time.perf_counter() - t0) * 1000.0
        except (EOFError, OSError, RuntimeError):
            return "engine_error", None, infos, (time.perf_counter() - t0) * 1000.0

        if line.startswith(f"info id {req_id} info64 "):
            try:
                token = line.split(" ", 4)[4]
                info_obj = json.loads(b64url_decode(token).decode("utf-8"))
                infos.append(info_obj)
            except Exception:
                pass
        elif line.startswith(f"bestmove id {req_id} "):
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            parts = line.split(" ")
            bestmove_obj = None
            if len(parts) == 5 and parts[3] == "move64":
                try:
                    bestmove_obj = json.loads(b64url_decode(parts[4]).decode("utf-8"))
                except Exception:
                    return "engine_error", None, infos, elapsed_ms
            return "ok", bestmove_obj, infos, elapsed_ms
        elif line.startswith("error "):
            return "engine_error", None, infos, (time.perf_counter() - t0) * 1000.0


def _match_legal_move(
    move_dict: Any, b: clp_board.Board, turn: clp_pieces.Player
) -> Optional[Tuple[clp_rules.Move, str]]:
    if not isinstance(move_dict, dict):
        return None
    required = {"schema", "kind", "src", "dst", "ptype", "facing", "rotate_after", "notation"}
    if set(move_dict.keys()) != required or move_dict.get("schema") != "clp.move/1":
        return None
    legal = clp_rules.legal_moves(b, turn)
    matched = [
        m
        for m in legal
        if m.kind == move_dict["kind"]
        and (list(m.src) if m.src is not None else None) == move_dict["src"]
        and (list(m.dst) if m.dst is not None else None) == move_dict["dst"]
        and (m.ptype.name if m.ptype is not None else "") == move_dict["ptype"]
        and m.facing == move_dict["facing"]
        and m.rotate_after == move_dict["rotate_after"]
    ]
    if len(matched) != 1:
        return None
    mv = matched[0]
    canon = mv.canonical_notation(b)
    if move_dict.get("notation") != canon:
        return None
    return mv, canon


def play_single_game(task: dict) -> dict:
    """Worker function playing one complete game in a subprocess pool."""
    game_id: int = task["game_id"]
    pair_id: int = task["pair_id"]
    opening_id: int = task["opening_id"]
    opening_lppn: str = task["opening_lppn"]
    opening_moves: List[str] = task["opening_moves"]
    run_name: str = task["run_name"]
    exe_a: Path = Path(task["exe_a"])
    exe_b: Path = Path(task["exe_b"])
    name_a: str = task["name_a"]
    name_b: str = task["name_b"]
    mt_a: int = task["mt_a"]
    mt_b: int = task["mt_b"]
    a_is_cam: bool = task["a_is_cam"]
    max_ply: int = task["max_ply"]
    grace_ms: int = task["grace_ms"]
    games_dir: Path = Path(task["games_dir"])

    cam_exe = exe_a if a_is_cam else exe_b
    xanh_exe = exe_b if a_is_cam else exe_a
    cam_name = name_a if a_is_cam else name_b
    xanh_name = name_b if a_is_cam else name_a
    cam_mt = mt_a if a_is_cam else mt_b
    xanh_mt = mt_b if a_is_cam else mt_a

    t_game0 = time.perf_counter()

    if opening_moves:
        state, b, turn = build_state_from_lppn_and_moves(STARTPOS_LPPN, opening_moves)
    else:
        state, b, turn = build_state_from_lppn(opening_lppn)

    rep_map: Dict[str, dict] = {
        core_bridge._position_identity_py(e["position"]): e
        for e in state["repetition"]["entries"]
    }

    stats: Dict[clp_pieces.Player, dict] = {
        clp_pieces.Player.CAM: {"nodes": 0, "time_ms": 0, "elapsed_ms": 0.0, "sum_depth": 0, "info_moves": 0},
        clp_pieces.Player.XANH: {"nodes": 0, "time_ms": 0, "elapsed_ms": 0.0, "sum_depth": 0, "info_moves": 0},
    }
    played_moves: List[str] = []

    cam_client = CLPEPClient(cam_exe)
    xanh_client = CLPEPClient(xanh_exe)
    winner_player: Optional[str] = None  # "CAM" | "XANH" | "DRAW"
    reason = "unknown"

    try:
        cam_client.start(do_handshake=True)
        _configure_engine(cam_client)
        xanh_client.start(do_handshake=True)
        _configure_engine(xanh_client)

        while True:
            res_obj = state.get("result", {})
            if res_obj.get("status") == "terminal":
                winner_player = res_obj["winner"]
                reason = res_obj["reason"]
                break

            if int(state["ply"]) >= max_ply:
                winner_player = "DRAW"
                reason = f"max_ply_{max_ply}"
                break

            active_client = cam_client if turn is clp_pieces.Player.CAM else xanh_client
            side_mt = cam_mt if turn is clp_pieces.Player.CAM else xanh_mt

            status, bestmove_obj, infos, elapsed_ms = _search_with_deadline(
                active_client, state, side_mt, grace_ms=grace_ms
            )

            mpv1_infos = [i for i in infos if i.get("multipv", 1) == 1]
            if mpv1_infos:
                last_info = mpv1_infos[-1]
                st_side = stats[turn]
                st_side["nodes"] += int(last_info.get("nodes", 0))
                st_side["time_ms"] += int(last_info.get("time_ms", 0))
                st_side["elapsed_ms"] += elapsed_ms
                st_side["sum_depth"] += int(last_info.get("depth", 0))
                st_side["info_moves"] += 1

            if status != "ok":
                winner_player = clp_rules.other(turn).name
                reason = status
                break

            matched = _match_legal_move(bestmove_obj, b, turn)
            if matched is None:
                winner_player = clp_rules.other(turn).name
                reason = "illegal_move"
                break

            mv, canon_not = matched
            played_moves.append(canon_not)

            clp_rules.apply_move(b, turn, mv)
            turn = clp_rules.other(turn)
            next_pos = core_bridge._encode_position_py(b, turn)
            state["position"] = next_pos
            state["ply"] = int(state["ply"]) + 1

            next_id = core_bridge._position_identity_py(next_pos)
            if next_id in rep_map:
                rep_map[next_id]["count"] += 1
            else:
                new_entry = {"count": 1, "position": copy.deepcopy(next_pos)}
                state["repetition"]["entries"].append(new_entry)
                rep_map[next_id] = new_entry

            core_bridge._adjudicate_state_py(state)
    finally:
        cam_client.close()
        xanh_client.close()

    elapsed_s = round(time.perf_counter() - t_game0, 3)

    if winner_player == "DRAW":
        winner_ab = "draw"
        cam_result = "draw"
        score_a = 0.5
    elif winner_player == "CAM":
        winner_ab = "A" if a_is_cam else "B"
        cam_result = "win"
        score_a = 1.0 if a_is_cam else 0.0
    elif winner_player == "XANH":
        winner_ab = "B" if a_is_cam else "A"
        cam_result = "loss"
        score_a = 0.0 if a_is_cam else 1.0
    else:
        raise RuntimeError(f"Unexpected winner_player={winner_player!r}")

    def _side_metrics(pl: clp_pieces.Player) -> Tuple[float, float, int, int]:
        s = stats[pl]
        n = s["nodes"]
        t_ms = s["time_ms"] if s["time_ms"] > 0 else int(round(s["elapsed_ms"]))
        nps = round((n * 1000.0) / t_ms, 1) if t_ms > 0 and n > 0 else 0.0
        avg_d = round(s["sum_depth"] / s["info_moves"], 3) if s["info_moves"] > 0 else 0.0
        return nps, avg_d, n, t_ms

    cam_nps, cam_depth, cam_nodes, cam_time_ms = _side_metrics(clp_pieces.Player.CAM)
    xanh_nps, xanh_depth, xanh_nodes, xanh_time_ms = _side_metrics(clp_pieces.Player.XANH)

    record = {
        "run": run_name,
        "id": game_id,
        "pair_id": pair_id,
        "opening_id": opening_id,
        "opening_lppn": opening_lppn,
        "engine_a": name_a,
        "engine_b": name_b,
        "cam_exe": cam_name,
        "xanh_exe": xanh_name,
        "cam_movetime_ms": cam_mt,
        "xanh_movetime_ms": xanh_mt,
        "winner": winner_ab,
        "cam_result": cam_result,
        "score_a": score_a,
        "reason": reason,
        "ply": int(state["ply"]),
        "moves_played": len(played_moves),
        "cam_nps": cam_nps,
        "xanh_nps": xanh_nps,
        "cam_avg_depth": cam_depth,
        "xanh_avg_depth": xanh_depth,
        "cam_nodes": cam_nodes,
        "cam_time_ms": cam_time_ms,
        "xanh_nodes": xanh_nodes,
        "xanh_time_ms": xanh_time_ms,
        "elapsed_s": elapsed_s,
    }

    game_txt = "\n".join(
        [
            f"# Game {game_id} | Run: {run_name} | Pair: {pair_id} | Opening: {opening_id}",
            f"# Cam: {cam_name} ({cam_mt} ms) | Xanh: {xanh_name} ({xanh_mt} ms)",
            f"LPPN: {opening_lppn}",
            f"Moves ({len(played_moves)}): {' '.join(played_moves)}",
            f"Result: {winner_player} ({reason}) | WinnerAB: {winner_ab} | Ply: {state['ply']}",
            f"Cam stats: avg_depth={cam_depth}, nps={cam_nps}",
            f"Xanh stats: avg_depth={xanh_depth}, nps={xanh_nps}",
        ]
    ) + "\n"
    (games_dir / f"{game_id:04d}.txt").write_text(game_txt, encoding="utf-8")
    return record


def _elo_from_score(mu: float) -> float:
    mu = min(max(mu, 1e-6), 1.0 - 1e-6)
    return -400.0 * math.log10((1.0 / mu) - 1.0)


def compute_stats(records: List[dict]) -> dict:
    n = len(records)
    if n == 0:
        return {"games": 0}

    wins_a = sum(1 for r in records if r["winner"] == "A")
    losses_a = sum(1 for r in records if r["winner"] == "B")
    draws = sum(1 for r in records if r["winner"] == "draw")
    score_a = wins_a + 0.5 * draws
    mu = score_a / n

    # Group by pair_id for paired 95% CI and LOS
    by_pair: Dict[int, List[dict]] = {}
    for r in records:
        by_pair.setdefault(int(r["pair_id"]), []).append(r)
    pair_scores = [
        sum(g["score_a"] for g in gs) / len(gs)
        for gs in by_pair.values()
        if len(gs) == 2
    ]

    if len(pair_scores) >= 2:
        p = len(pair_scores)
        mean_p = sum(pair_scores) / p
        var_p = sum((y - mean_p) ** 2 for y in pair_scores) / (p - 1)
        se = math.sqrt(var_p / p)
        ci_method = "paired"
    else:
        p = len(pair_scores)
        mean_p = mu
        var_p = sum((r["score_a"] - mu) ** 2 for r in records) / max(1, n - 1)
        se = math.sqrt(var_p / n)
        ci_method = "unpaired"

    elo = _elo_from_score(mean_p)
    if se > 0.0:
        elo_low = _elo_from_score(mean_p - 1.96 * se)
        elo_high = _elo_from_score(mean_p + 1.96 * se)
        half_ci = 0.5 * (elo_high - elo_low)
        z = (mean_p - 0.5) / se
        los = 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))
    else:
        elo_low = elo
        elo_high = elo
        half_ci = 0.0
        los = 1.0 if mean_p > 0.5 else (0.0 if mean_p < 0.5 else 0.5)

    reasons: Dict[str, int] = {}
    for r in records:
        reasons[r["reason"]] = reasons.get(r["reason"], 0) + 1

    # Aggregate NPS and avg_depth per engine A / B
    a_nodes = a_time = b_nodes = b_time = 0
    a_depth_sum = b_depth_sum = 0.0
    a_games_d = b_games_d = 0
    for r in records:
        a_is_cam = r["cam_exe"] == r["engine_a"] and (
            r["engine_a"] != r["engine_b"] or r["id"] % 2 == 0
        )
        if a_is_cam:
            a_nodes += r["cam_nodes"]
            a_time += r["cam_time_ms"]
            b_nodes += r["xanh_nodes"]
            b_time += r["xanh_time_ms"]
            if r["cam_avg_depth"] > 0:
                a_depth_sum += r["cam_avg_depth"]
                a_games_d += 1
            if r["xanh_avg_depth"] > 0:
                b_depth_sum += r["xanh_avg_depth"]
                b_games_d += 1
        else:
            b_nodes += r["cam_nodes"]
            b_time += r["cam_time_ms"]
            a_nodes += r["xanh_nodes"]
            a_time += r["xanh_time_ms"]
            if r["xanh_avg_depth"] > 0:
                a_depth_sum += r["xanh_avg_depth"]
                a_games_d += 1
            if r["cam_avg_depth"] > 0:
                b_depth_sum += r["cam_avg_depth"]
                b_games_d += 1

    return {
        "games": n,
        "complete_pairs": len(pair_scores),
        "ci_method": ci_method,
        "engine_a": records[0]["engine_a"],
        "engine_b": records[0]["engine_b"],
        "wins_a": wins_a,
        "draws": draws,
        "losses_a": losses_a,
        "score_a": score_a,
        "score_pct": round(100.0 * mean_p, 2),
        "elo": round(elo, 2),
        "elo_ci95": round(half_ci, 2),
        "elo_low": round(elo_low, 2),
        "elo_high": round(elo_high, 2),
        "los_pct": round(100.0 * los, 2),
        "avg_ply": round(sum(r["ply"] for r in records) / n, 1),
        "avg_moves_played": round(sum(r["moves_played"] for r in records) / n, 1),
        "a_nps": round((a_nodes * 1000.0) / a_time, 1) if a_time > 0 else 0.0,
        "b_nps": round((b_nodes * 1000.0) / b_time, 1) if b_time > 0 else 0.0,
        "a_avg_depth": round(a_depth_sum / a_games_d, 2) if a_games_d > 0 else 0.0,
        "b_avg_depth": round(b_depth_sum / b_games_d, 2) if b_games_d > 0 else 0.0,
        "reasons": reasons,
        "illegal_moves": reasons.get("illegal_move", 0),
        "timeouts": reasons.get("timeout", 0),
        "engine_errors": reasons.get("engine_error", 0),
    }


def print_stats(summary: dict, wall_s: Optional[float] = None) -> None:
    if summary.get("games", 0) == 0:
        print("No games recorded.")
        return
    print(
        f"=== Match Summary: {summary['engine_a']} vs {summary['engine_b']} "
        f"({summary['games']} games, {summary['complete_pairs']} pairs [{summary['ci_method']}]) ==="
    )
    print(
        f"W/D/L (A): {summary['wins_a']}/{summary['draws']}/{summary['losses_a']} | "
        f"Score: {summary['score_a']:.1f}/{summary['games']} ({summary['score_pct']:.2f}%)"
    )
    print(
        f"Elo: {summary['elo']:+.1f} ± {summary['elo_ci95']:.1f} "
        f"[{summary['elo_low']:+.1f}, {summary['elo_high']:+.1f}] | LOS: {summary['los_pct']:.2f}%"
    )
    print(
        f"Avg ply: {summary['avg_ply']} (played: {summary['avg_moves_played']}) | "
        f"Illegal: {summary['illegal_moves']} | Timeouts: {summary['timeouts']} | Errors: {summary['engine_errors']}"
    )
    print(
        f"A stats: depth={summary['a_avg_depth']}, nps={summary['a_nps']:.0f} | "
        f"B stats: depth={summary['b_avg_depth']}, nps={summary['b_nps']:.0f}"
    )
    print(f"Reasons: {summary['reasons']}")
    if wall_s is not None:
        print(f"Wall time: {wall_s:.1f}s")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run CLP-EP v1 engine-vs-engine match.")
    parser.add_argument("--run", required=True, help="Run name (used for games/<run>/).")
    parser.add_argument("--engine-a", type=Path, required=True, help="Path to Engine A exe.")
    parser.add_argument("--engine-b", type=Path, required=True, help="Path to Engine B exe.")
    parser.add_argument("--name-a", default=None, help="Display name for Engine A.")
    parser.add_argument("--name-b", default=None, help="Display name for Engine B.")
    parser.add_argument("--games", type=int, default=300, help="Total games (even number preferred).")
    parser.add_argument("--movetime", type=int, default=100, help="Fixed movetime per move (ms).")
    parser.add_argument("--movetime-a", type=int, default=None, help="Override movetime for Engine A (ms).")
    parser.add_argument("--movetime-b", type=int, default=None, help="Override movetime for Engine B (ms).")
    parser.add_argument("--grace-ms", type=int, default=1000, help="Timeout grace beyond movetime (ms).")
    parser.add_argument("--max-ply", type=int, default=300, help="Draw at ply >= max-ply.")
    parser.add_argument("--openings", type=Path, default=HOMEI_ROOT / "openings.txt")
    parser.add_argument(
        "--results-jsonl",
        type=Path,
        default=HOMEI_ROOT / "results.jsonl",
        help="Master JSONL file appending all completed games.",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=max(1, (os.cpu_count() or 1) // 2 - 1),
        help="Number of concurrent games.",
    )
    parser.add_argument("--stats-only", action="store_true", help="Only print stats for existing run.")
    args = parser.parse_args()

    run_dir = HOMEI_ROOT / "games" / args.run
    run_dir.mkdir(parents=True, exist_ok=True)
    run_jsonl = run_dir / "results.jsonl"

    existing: Dict[int, dict] = {}
    if run_jsonl.is_file():
        for raw in run_jsonl.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
                existing[int(obj["id"])] = obj
            except Exception:
                continue

    if args.stats_only:
        records = [existing[k] for k in sorted(existing) if k < args.games]
        summary = compute_stats(records)
        print_stats(summary)
        return 0

    openings = load_openings(args.openings)
    name_a = args.name_a or args.engine_a.stem
    name_b = args.name_b or args.engine_b.stem
    mt_a = args.movetime_a if args.movetime_a is not None else args.movetime
    mt_b = args.movetime_b if args.movetime_b is not None else args.movetime

    pending_tasks: List[dict] = []
    for gid in range(args.games):
        if gid in existing:
            continue
        pair_id = gid // 2
        opening_id = pair_id % len(openings)
        opening_lppn, opening_moves = openings[opening_id]
        a_is_cam = (gid % 2 == 0)
        pending_tasks.append(
            {
                "game_id": gid,
                "pair_id": pair_id,
                "opening_id": opening_id,
                "opening_lppn": opening_lppn,
                "opening_moves": opening_moves,
                "run_name": args.run,
                "exe_a": str(args.engine_a.resolve()),
                "exe_b": str(args.engine_b.resolve()),
                "name_a": name_a,
                "name_b": name_b,
                "mt_a": mt_a,
                "mt_b": mt_b,
                "a_is_cam": a_is_cam,
                "max_ply": args.max_ply,
                "grace_ms": args.grace_ms,
                "games_dir": str(run_dir),
            }
        )

    print(
        f"[match] Run={args.run} | {name_a} ({mt_a}ms) vs {name_b} ({mt_b}ms) | "
        f"target={args.games}, resumed={len(existing)}, pending={len(pending_tasks)}, workers={args.workers}"
    )

    t0 = time.perf_counter()
    if pending_tasks:
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
            futures = {pool.submit(play_single_game, t): t["game_id"] for t in pending_tasks}
            done_now = 0
            for fut in concurrent.futures.as_completed(futures):
                rec = fut.result()
                existing[int(rec["id"])] = rec
                line = json.dumps(rec, ensure_ascii=False) + "\n"
                with run_jsonl.open("a", encoding="utf-8") as f:
                    f.write(line)
                    f.flush()
                with args.results_jsonl.open("a", encoding="utf-8") as mf:
                    mf.write(line)
                    mf.flush()
                done_now += 1
                if done_now % 20 == 0 or done_now == len(pending_tasks):
                    recs = [existing[k] for k in sorted(existing) if k < args.games]
                    s = compute_stats(recs)
                    dt = time.perf_counter() - t0
                    print(
                        f"[progress {len(recs)}/{args.games}] W/D/L={s['wins_a']}/{s['draws']}/{s['losses_a']} "
                        f"Elo={s['elo']:+.1f}±{s['elo_ci95']:.1f} LOS={s['los_pct']:.1f}% "
                        f"({dt:.1f}s)"
                    )

    wall_s = time.perf_counter() - t0
    final_records = [existing[k] for k in sorted(existing) if k < args.games]
    summary = compute_stats(final_records)
    print_stats(summary, wall_s=wall_s)
    (run_dir / "summary.json").write_text(
        json.dumps({**summary, "wall_s": round(wall_s, 2)}, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Headless CLP-EP v1 match runner for Homei engines using ref.clp as referee and SafeEngineClient."""

from __future__ import annotations

import argparse
import concurrent.futures
import copy
from datetime import datetime
import json
import math
import os
import re
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
    b64url_decode,
    build_state_from_lppn,
    build_state_from_lppn_and_moves,
)
from ref.clp import board as clp_board
from ref.clp import core_bridge
from ref.clp import lppn as clp_lppn
from ref.clp import pieces as clp_pieces
from ref.clp import record_io
from ref.clp import rules as clp_rules
from tools.safe_proc import (
    HeartbeatMonitor,
    HeavyLock,
    SafeEngineClient,
    check_free_disk,
    max_safe_workers,
)

EVE_LPGF_ROOT = Path(r"E:\all-code\Code1\CLP_dataset\LPGF\EvE\Arena 4")


def _startpos_lppn() -> str:
    b = clp_board.Board()
    b.setup_initial()
    return clp_lppn.to_lppn(b, clp_pieces.Player.CAM, ply=0, strict=True)


STARTPOS_LPPN = _startpos_lppn()


def parse_env_arg(env_str: Optional[str]) -> Dict[str, str]:
    """Parse 'KEY1=VAL1;KEY2=VAL2' into a dict."""
    if not env_str:
        return {}
    out: Dict[str, str] = {}
    for part in env_str.split(";"):
        part = part.strip()
        if not part or "=" not in part:
            continue
        k, v = part.split("=", 1)
        out[k.strip()] = v.strip()
    return out


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


def _find_king_rc(b: clp_board.Board, owner: clp_pieces.Player) -> Optional[Tuple[int, int]]:
    for r in range(9):
        for c in range(9):
            sq = b.sq(r, c)
            for p in sq.stack:
                if p.ptype == clp_pieces.PieceType.VUONG and p.owner == owner:
                    return (r, c)
    return None


def _has_siege_ray_py(
    b: clp_board.Board,
    sq_rc: Tuple[int, int],
    slider_ptype: str,
    slider_facing: int,
    ek_rc: Optional[Tuple[int, int]],
) -> bool:
    if ek_rc is None:
        return False
    ekr, ekc = ek_rc
    r, c = sq_rc
    if slider_ptype == "LONG":
        dirs = [(-1, 0), (1, 0), (0, -1), (0, 1)]
    elif slider_ptype == "PHUONG":
        dirs = [(-1, -1), (-1, 1), (1, -1), (1, 1)]
    elif slider_ptype == "XA":
        dmap = {0: (-1, 0), 1: (0, 1), 2: (1, 0), 3: (0, -1)}
        dirs = [dmap.get(slider_facing % 4, (-1, 0))]
    else:
        return False

    for dr, dc in dirs:
        tr, tc = r + dr, c + dc
        while 0 <= tr < 9 and 0 <= tc < 9:
            cell = b.sq(tr, tc)
            in_kz = abs(tr - ekr) <= 1 and abs(tc - ekc) <= 1
            if cell.is_empty:
                if in_kz:
                    return True
            else:
                if tr == ekr and tc == ekc:
                    return True
                break
            tr += dr
            tc += dc
    return False


def compute_board_koma_snapshot(b: clp_board.Board, side: clp_pieces.Player) -> Tuple[int, int, int, int]:
    """Compute (edge_facing_binh_cnt, buried_slider_cnt, has_2_adv_def, has_siege_3x3) for `side` on board `b`."""
    enemy = clp_rules.other(side)
    ek_rc = _find_king_rc(b, enemy)
    edge_binh = 0
    buried = 0
    adv_def_cnt = 0
    siege_units = 0

    for r in range(9):
        rel_row = r if side == clp_pieces.Player.CAM else (8 - r)
        for c in range(9):
            sq = b.sq(r, c)
            if sq.is_empty or sq.top.owner != side:
                continue
            top_p = sq.top
            pt_name = top_p.ptype.name
            f = top_p.facing % 4

            # 1. Edge-facing Binh
            if pt_name == "BINH":
                if (f == 0 and r == 0) or (f == 1 and c == 8) or (f == 2 and r == 8) or (f == 3 and c == 0):
                    edge_binh += 1

            # 2. Buried slider (own Long/Phượng/Xa under Binh/Sĩ/Tướng in home half or without siege ray)
            if len(sq.stack) >= 2 and pt_name in ("BINH", "SI", "TUONG"):
                for under_p in sq.stack[:-1]:
                    if under_p.ptype.name in ("LONG", "PHUONG", "XA"):
                        has_ray = _has_siege_ray_py(b, (r, c), under_p.ptype.name, under_p.facing, ek_rc)
                        if rel_row <= 3 or not has_ray:
                            buried += 1

            # 3. Sĩ/Kị/Tướng on active middle ranks d, e, f (r in 3, 4, 5)
            if r in (3, 4, 5):
                for p in sq.stack:
                    if p.ptype.name in ("SI", "KI", "TUONG"):
                        adv_def_cnt += 1

            # 4. Siege pressure on enemy King 3x3 zone
            if ek_rc is not None and pt_name != "VUONG":
                ekr, ekc = ek_rc
                manh_ek = abs(r - ekr) + abs(c - ekc)
                if manh_ek <= 3:
                    faces_ek = (
                        pt_name in ("LONG", "PHUONG")
                        or (f == 0 and ekr < r)
                        or (f == 2 and ekr > r)
                        or (f == 1 and ekc > c)
                        or (f == 3 and ekc < c)
                    )
                    if faces_ek:
                        siege_units += 2 + (2 if len(sq.stack) >= 2 else 0)
                else:
                    if pt_name in ("LONG", "PHUONG", "XA") and _has_siege_ray_py(b, (r, c), pt_name, f, ek_rc):
                        siege_units += 2
                    elif len(sq.stack) >= 2:
                        sec = sq.stack[-2]
                        if sec.ptype.name in ("LONG", "PHUONG", "XA") and _has_siege_ray_py(
                            b, (r, c), sec.ptype.name, sec.facing, ek_rc
                        ):
                            siege_units += 2

    return edge_binh, buried, (1 if adv_def_cnt >= 2 else 0), (1 if siege_units >= 4 else 0)


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


def format_lpgf_v2_move_line(idx: int, mv: clp_rules.Move, canon_not: str) -> str:
    """Format move `idx` for LPGF v2.0 with parenthesized source square."""
    if mv.kind == "drop" or mv.src is None:
        return f"{idx}.{canon_not}"
    base = re.sub(r"\.[a-i][1-9]$", "", canon_not)
    src_sq = record_io.pos_to_sq(mv.src)
    return f"{idx}.{base}({src_sq})"


def write_lpgf_v2_game_file(
    lpgf_path: Path,
    run_name: str,
    game_id: int,
    cam_name: str,
    xanh_name: str,
    winner_player: Optional[str],
    reason: str,
    opening_board: clp_board.Board,
    opening_turn: clp_pieces.Player,
    opening_ply: int,
    lpgf_move_lines: List[str],
) -> None:
    now_str = datetime.now().strftime("%d-%m-%Y %H:%M")
    if winner_player == "CAM":
        winner_str = f"{cam_name} (Cam - {reason})"
    elif winner_player == "XANH":
        winner_str = f"{xanh_name} (Xanh - {reason})"
    else:
        winner_str = f"Hòa ({reason})"

    lines = [
        "#lpgf version=2.0 encoding=UTF-8",
        f"Tên Phòng: EvE {run_name} #{game_id + 1:04d}",
        f"Ngày Giờ Chơi: {now_str}",
        f"Ngày Giờ Lưu: {now_str}",
        f"Kỳ Thủ Thắng: {winner_str}",
        f"Kỳ Thủ Cầm Cam: {cam_name}",
        f"Kỳ Thủ Cầm Xanh: {xanh_name}",
        "",
    ]
    # Only embed ASCII diagram when starting from a non-standard position (matches LPGF 2.0 spec & GUI export)
    if opening_ply != 0 or clp_lppn.to_lppn(opening_board, opening_turn, ply=0) != STARTPOS_LPPN:
        lines.append(record_io.format_ascii_board(opening_board, opening_turn, opening_ply))
        lines.append("")
    lines.extend(lpgf_move_lines)
    lines.append("")
    lpgf_path.parent.mkdir(parents=True, exist_ok=True)
    lpgf_path.write_text("\n".join(lines), encoding="utf-8")


def play_single_game(task: dict) -> dict:
    """Worker function playing one complete game in a subprocess pool via SafeEngineClient."""
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
    env_a: Dict[str, str] = task.get("env_a") or {}
    env_b: Dict[str, str] = task.get("env_b") or {}
    mt_a: int = task["mt_a"]
    mt_b: int = task["mt_b"]
    a_is_cam: bool = task["a_is_cam"]
    max_ply: int = task["max_ply"]
    grace_ms: int = task["grace_ms"]
    games_dir: Path = Path(task["games_dir"])
    lpgf_dir: Path = Path(task["lpgf_dir"])

    cam_exe = exe_a if a_is_cam else exe_b
    xanh_exe = exe_b if a_is_cam else exe_a
    cam_name = name_a if a_is_cam else name_b
    xanh_name = name_b if a_is_cam else name_a
    cam_env = env_a if a_is_cam else env_b
    xanh_env = env_b if a_is_cam else env_a
    cam_mt = mt_a if a_is_cam else mt_b
    xanh_mt = mt_b if a_is_cam else mt_a

    t_game0 = time.perf_counter()

    if opening_moves:
        state, b, turn = build_state_from_lppn_and_moves(STARTPOS_LPPN, opening_moves)
    else:
        state, b, turn = build_state_from_lppn(opening_lppn)

    opening_board = b.clone()
    opening_turn = turn
    opening_ply = int(state["ply"])

    rep_map: Dict[str, dict] = {
        core_bridge._position_identity_py(e["position"]): e
        for e in state["repetition"]["entries"]
    }

    stats: Dict[clp_pieces.Player, dict] = {
        clp_pieces.Player.CAM: {
            "nodes": 0,
            "time_ms": 0,
            "elapsed_ms": 0.0,
            "sum_depth": 0,
            "info_moves": 0,
            "over_mt_200ms": 0,
            "koma_plies": 0,
            "edge_binh_sum": 0,
            "buried_slider_sum": 0,
            "adv_def_sum": 0,
            "siege_3x3_sum": 0,
        },
        clp_pieces.Player.XANH: {
            "nodes": 0,
            "time_ms": 0,
            "elapsed_ms": 0.0,
            "sum_depth": 0,
            "info_moves": 0,
            "over_mt_200ms": 0,
            "koma_plies": 0,
            "edge_binh_sum": 0,
            "buried_slider_sum": 0,
            "adv_def_sum": 0,
            "siege_3x3_sum": 0,
        },
    }
    played_moves: List[str] = []
    lpgf_move_lines: List[str] = []

    winner_player: Optional[str] = None  # "CAM" | "XANH" | "DRAW"
    reason = "unknown"

    with SafeEngineClient(cam_exe, extra_env=cam_env) as cam_client, SafeEngineClient(
        xanh_exe, extra_env=xanh_env
    ) as xanh_client:
        cam_client.prepare_new_game(hash_mb=64, threads=1)
        xanh_client.prepare_new_game(hash_mb=64, threads=1)

        while True:
            res_obj = state.get("result", {})
            if res_obj.get("status") == "terminal":
                winner_player = res_obj.get("winner") or "DRAW"
                reason = res_obj["reason"]
                break

            if int(state["ply"]) >= max_ply:
                winner_player = "DRAW"
                reason = f"max_ply_{max_ply}"
                break

            active_client = cam_client if turn is clp_pieces.Player.CAM else xanh_client
            side_mt = cam_mt if turn is clp_pieces.Player.CAM else xanh_mt

            out = active_client.search_movetime(state, movetime_ms=side_mt, grace_ms=grace_ms)
            status = out.status
            bestmove_obj = out.bestmove
            infos = out.infos
            elapsed_ms = out.elapsed_ms

            st_side = stats[turn]
            if elapsed_ms > side_mt + 200.0:
                st_side["over_mt_200ms"] += 1

            mpv1_infos = [i for i in infos if i.get("multipv", 1) == 1]
            if mpv1_infos:
                last_info = mpv1_infos[-1]
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
            lpgf_move_lines.append(format_lpgf_v2_move_line(len(played_moves), mv, canon_not))

            mover = turn
            prev_board = b.clone() if any(int(e["count"]) >= 2 for e in rep_map.values()) else None
            clp_rules.apply_move(b, turn, mv)
            turn = clp_rules.other(turn)
            next_ply = int(state["ply"]) + 1

            if 12 <= next_ply <= 40:
                eb, bs, ad, sg = compute_board_koma_snapshot(b, mover)
                st_mover = stats[mover]
                st_mover["koma_plies"] += 1
                st_mover["edge_binh_sum"] += eb
                st_mover["buried_slider_sum"] += bs
                st_mover["adv_def_sum"] += ad
                st_mover["siege_3x3_sum"] += sg

            next_pos = core_bridge._encode_position_py(b, turn)
            state["position"] = next_pos
            state["ply"] = next_ply

            next_id = core_bridge._position_identity_py(next_pos)
            unavoidable = False
            if next_id in rep_map:
                if rep_map[next_id]["count"] >= 2 and prev_board is not None:
                    unavoidable = core_bridge._all_legal_moves_lead_to_rep3_py(
                        prev_board, mover, state["repetition"]["entries"]
                    )
                rep_map[next_id]["count"] += 1
            else:
                new_entry = {"count": 1, "position": copy.deepcopy(next_pos)}
                state["repetition"]["entries"].append(new_entry)
                rep_map[next_id] = new_entry

            core_bridge._adjudicate_state_py(state, unavoidable_rep3=unavoidable)

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

    def _side_metrics(pl: clp_pieces.Player) -> dict:
        s = stats[pl]
        n = s["nodes"]
        t_ms = s["time_ms"] if s["time_ms"] > 0 else int(round(s["elapsed_ms"]))
        nps = round((n * 1000.0) / t_ms, 1) if t_ms > 0 and n > 0 else 0.0
        avg_d = round(s["sum_depth"] / s["info_moves"], 3) if s["info_moves"] > 0 else 0.0
        kp = s["koma_plies"]
        return {
            "nps": nps,
            "avg_depth": avg_d,
            "nodes": n,
            "time_ms": t_ms,
            "info_moves": s["info_moves"],
            "over_mt_200ms": s["over_mt_200ms"],
            "koma_plies": kp,
            "edge_binh_sum": s["edge_binh_sum"],
            "buried_slider_sum": s["buried_slider_sum"],
            "adv_def_sum": s["adv_def_sum"],
            "siege_3x3_sum": s["siege_3x3_sum"],
        }

    cam_m = _side_metrics(clp_pieces.Player.CAM)
    xanh_m = _side_metrics(clp_pieces.Player.XANH)

    lpgf_file = lpgf_dir / f"game_{game_id + 1:04d}.lpgf"
    write_lpgf_v2_game_file(
        lpgf_path=lpgf_file,
        run_name=run_name,
        game_id=game_id,
        cam_name=cam_name,
        xanh_name=xanh_name,
        winner_player=winner_player,
        reason=reason,
        opening_board=opening_board,
        opening_turn=opening_turn,
        opening_ply=opening_ply,
        lpgf_move_lines=lpgf_move_lines,
    )

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
        "cam_nps": cam_m["nps"],
        "xanh_nps": xanh_m["nps"],
        "cam_avg_depth": cam_m["avg_depth"],
        "xanh_avg_depth": xanh_m["avg_depth"],
        "cam_nodes": cam_m["nodes"],
        "cam_time_ms": cam_m["time_ms"],
        "xanh_nodes": xanh_m["nodes"],
        "xanh_time_ms": xanh_m["time_ms"],
        "cam_info_moves": cam_m["info_moves"],
        "xanh_info_moves": xanh_m["info_moves"],
        "cam_over_mt_200ms": cam_m["over_mt_200ms"],
        "xanh_over_mt_200ms": xanh_m["over_mt_200ms"],
        "cam_koma_plies": cam_m["koma_plies"],
        "xanh_koma_plies": xanh_m["koma_plies"],
        "cam_edge_binh_sum": cam_m["edge_binh_sum"],
        "xanh_edge_binh_sum": xanh_m["edge_binh_sum"],
        "cam_buried_slider_sum": cam_m["buried_slider_sum"],
        "xanh_buried_slider_sum": xanh_m["buried_slider_sum"],
        "cam_adv_def_sum": cam_m["adv_def_sum"],
        "xanh_adv_def_sum": xanh_m["adv_def_sum"],
        "cam_siege_3x3_sum": cam_m["siege_3x3_sum"],
        "xanh_siege_3x3_sum": xanh_m["siege_3x3_sum"],
        "lpgf_path": str(lpgf_file),
        "elapsed_s": elapsed_s,
    }

    game_txt = "\n".join(
        [
            f"# Game {game_id} | Run: {run_name} | Pair: {pair_id} | Opening: {opening_id}",
            f"# Cam: {cam_name} ({cam_mt} ms) | Xanh: {xanh_name} ({xanh_mt} ms)",
            f"LPPN: {opening_lppn}",
            f"Moves ({len(played_moves)}): {' '.join(played_moves)}",
            f"Result: {winner_player} ({reason}) | WinnerAB: {winner_ab} | Ply: {state['ply']}",
            f"Cam stats: avg_depth={cam_m['avg_depth']}, nps={cam_m['nps']}",
            f"Xanh stats: avg_depth={xanh_m['avg_depth']}, nps={xanh_m['nps']}",
        ]
    ) + "\n"
    (games_dir / f"{game_id:04d}.txt").write_text(game_txt, encoding="utf-8")
    return record


def _elo_from_score(mu: float) -> float:
    mu = min(max(mu, 1e-6), 1.0 - 1e-6)
    return -400.0 * math.log10((1.0 / mu) - 1.0)


def _score_from_elo(elo: float) -> float:
    return 1.0 / (1.0 + math.pow(10.0, -elo / 400.0))


def compute_pentanomial_sprt(
    records: List[dict],
    elo0: float = 0.0,
    elo1: float = 10.0,
    alpha: float = 0.05,
    beta: float = 0.05,
    min_pairs: int = 10,
) -> dict:
    """Compute Fishtest/OpenBench pentanomial SPRT statistics from paired games."""
    by_pair: Dict[int, List[dict]] = {}
    for r in records:
        by_pair.setdefault(int(r["pair_id"]), []).append(r)

    buckets = {"LL": 0, "LD": 0, "WL_DD": 0, "WD": 0, "WW": 0}
    pair_x: List[float] = []
    for gs in by_pair.values():
        if len(gs) != 2:
            continue
        pair_sum = gs[0]["score_a"] + gs[1]["score_a"]
        idx = int(round(pair_sum * 2.0))
        idx = min(max(idx, 0), 4)
        key = ("LL", "LD", "WL_DD", "WD", "WW")[idx]
        buckets[key] += 1
        pair_x.append(pair_sum * 0.5)

    n_pairs = len(pair_x)
    la = math.log(beta / (1.0 - alpha))
    lb = math.log((1.0 - beta) / alpha)
    mu0 = _score_from_elo(elo0)
    mu1 = _score_from_elo(elo1)

    if n_pairs == 0:
        return {
            "pairs": 0,
            "pentanomial": buckets,
            "elo0": elo0,
            "elo1": elo1,
            "alpha": alpha,
            "beta": beta,
            "la": round(la, 4),
            "lb": round(lb, 4),
            "llr": 0.0,
            "decision": "inconclusive",
        }

    mean_x = sum(pair_x) / n_pairs
    # Regularize variance slightly (+0.0625 total spread) to prevent zero division when early pairs are identical
    ss = sum((x - mean_x) ** 2 for x in pair_x)
    var_x = (ss + 0.0625) / n_pairs
    llr = (n_pairs * (mu1 - mu0) * (2.0 * mean_x - mu0 - mu1)) / (2.0 * var_x)

    decision = "inconclusive"
    if n_pairs >= min_pairs:
        if llr >= lb:
            decision = "H1"
        elif llr <= la:
            decision = "H0"

    return {
        "pairs": n_pairs,
        "pentanomial": buckets,
        "elo0": elo0,
        "elo1": elo1,
        "alpha": alpha,
        "beta": beta,
        "la": round(la, 4),
        "lb": round(lb, 4),
        "llr": round(llr, 4),
        "decision": decision,
    }


def compute_stats(
    records: List[dict],
    elo0: float = 0.0,
    elo1: float = 10.0,
    alpha: float = 0.05,
    beta: float = 0.05,
    sprt_min_pairs: int = 10,
) -> dict:
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

    # Aggregate NPS, avg_depth, and Koma secondary metrics per engine A / B
    a_nodes = a_time = b_nodes = b_time = 0
    a_depth_sum = b_depth_sum = 0.0
    a_games_d = b_games_d = 0
    a_over_200 = b_over_200 = 0
    a_moves_tot = b_moves_tot = 0
    a_kplies = b_kplies = 0
    a_eb = b_eb = 0
    a_bs = b_bs = 0
    a_ad = b_ad = 0
    a_sg = b_sg = 0

    for r in records:
        a_is_cam = r["cam_exe"] == r["engine_a"] and (
            r["engine_a"] != r["engine_b"] or r["id"] % 2 == 0
        )
        if a_is_cam:
            a_nodes += r["cam_nodes"]
            a_time += r["cam_time_ms"]
            b_nodes += r["xanh_nodes"]
            b_time += r["xanh_time_ms"]
            a_over_200 += int(r.get("cam_over_mt_200ms", 0))
            b_over_200 += int(r.get("xanh_over_mt_200ms", 0))
            a_moves_tot += int(r.get("cam_info_moves", 0))
            b_moves_tot += int(r.get("xanh_info_moves", 0))
            a_kplies += int(r.get("cam_koma_plies", 0))
            b_kplies += int(r.get("xanh_koma_plies", 0))
            a_eb += int(r.get("cam_edge_binh_sum", 0))
            b_eb += int(r.get("xanh_edge_binh_sum", 0))
            a_bs += int(r.get("cam_buried_slider_sum", 0))
            b_bs += int(r.get("xanh_buried_slider_sum", 0))
            a_ad += int(r.get("cam_adv_def_sum", 0))
            b_ad += int(r.get("xanh_adv_def_sum", 0))
            a_sg += int(r.get("cam_siege_3x3_sum", 0))
            b_sg += int(r.get("xanh_siege_3x3_sum", 0))
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
            b_over_200 += int(r.get("cam_over_mt_200ms", 0))
            a_over_200 += int(r.get("xanh_over_mt_200ms", 0))
            b_moves_tot += int(r.get("cam_info_moves", 0))
            a_moves_tot += int(r.get("xanh_info_moves", 0))
            b_kplies += int(r.get("cam_koma_plies", 0))
            a_kplies += int(r.get("xanh_koma_plies", 0))
            b_eb += int(r.get("cam_edge_binh_sum", 0))
            a_eb += int(r.get("xanh_edge_binh_sum", 0))
            b_bs += int(r.get("cam_buried_slider_sum", 0))
            a_bs += int(r.get("xanh_buried_slider_sum", 0))
            b_ad += int(r.get("cam_adv_def_sum", 0))
            a_ad += int(r.get("xanh_adv_def_sum", 0))
            b_sg += int(r.get("cam_siege_3x3_sum", 0))
            a_sg += int(r.get("xanh_siege_3x3_sum", 0))
            if r["xanh_avg_depth"] > 0:
                a_depth_sum += r["xanh_avg_depth"]
                a_games_d += 1
            if r["cam_avg_depth"] > 0:
                b_depth_sum += r["cam_avg_depth"]
                b_games_d += 1

    sprt_info = compute_pentanomial_sprt(
        records,
        elo0=elo0,
        elo1=elo1,
        alpha=alpha,
        beta=beta,
        min_pairs=sprt_min_pairs,
    )

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
        "pentanomial": sprt_info["pentanomial"],
        "sprt": sprt_info,
        "avg_ply": round(sum(r["ply"] for r in records) / n, 1),
        "avg_moves_played": round(sum(r["moves_played"] for r in records) / n, 1),
        "a_nps": round((a_nodes * 1000.0) / a_time, 1) if a_time > 0 else 0.0,
        "b_nps": round((b_nodes * 1000.0) / b_time, 1) if b_time > 0 else 0.0,
        "a_avg_depth": round(a_depth_sum / a_games_d, 2) if a_games_d > 0 else 0.0,
        "b_avg_depth": round(b_depth_sum / b_games_d, 2) if b_games_d > 0 else 0.0,
        "a_over_mt_200ms": a_over_200,
        "b_over_mt_200ms": b_over_200,
        "a_moves_total": a_moves_tot,
        "b_moves_total": b_moves_tot,
        "a_edge_facing_binh": round(a_eb / max(1, a_kplies), 4),
        "b_edge_facing_binh": round(b_eb / max(1, b_kplies), 4),
        "a_buried_slider": round(a_bs / max(1, a_kplies), 4),
        "b_buried_slider": round(b_bs / max(1, b_kplies), 4),
        "a_adv_def_rate_pct": round(100.0 * a_ad / max(1, a_kplies), 2),
        "b_adv_def_rate_pct": round(100.0 * b_ad / max(1, b_kplies), 2),
        "a_siege_3x3_rate_pct": round(100.0 * a_sg / max(1, a_kplies), 2),
        "b_siege_3x3_rate_pct": round(100.0 * b_sg / max(1, b_kplies), 2),
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
    if "sprt" in summary:
        sp = summary["sprt"]
        pt = sp["pentanomial"]
        print(
            f"Ptnml (0/0.5/1/1.5/2): [{pt['LL']}, {pt['LD']}, {pt['WL_DD']}, {pt['WD']}, {pt['WW']}] | "
            f"SPRT ({sp['elo0']:+.1f}, {sp['elo1']:+.1f}): LLR={sp['llr']:+.2f} "
            f"[{sp['la']:+.2f}, {sp['lb']:+.2f}] -> {sp['decision']}"
        )
    print(
        f"Avg ply: {summary['avg_ply']} (played: {summary['avg_moves_played']}) | "
        f"Illegal: {summary['illegal_moves']} | Timeouts: {summary['timeouts']} | Errors: {summary['engine_errors']}"
    )
    print(
        f"A stats: depth={summary['a_avg_depth']}, nps={summary['a_nps']:.0f}, over200ms={summary['a_over_mt_200ms']} | "
        f"B stats: depth={summary['b_avg_depth']}, nps={summary['b_nps']:.0f}, over200ms={summary['b_over_mt_200ms']}"
    )
    print(
        f"Koma metrics (ply 12..40) A vs B: "
        f"edge_binh={summary['a_edge_facing_binh']:.4f} vs {summary['b_edge_facing_binh']:.4f} | "
        f"buried_slider={summary['a_buried_slider']:.4f} vs {summary['b_buried_slider']:.4f} | "
        f"adv_def_rate={summary['a_adv_def_rate_pct']:.2f}% vs {summary['b_adv_def_rate_pct']:.2f}% | "
        f"siege_3x3_rate={summary['a_siege_3x3_rate_pct']:.2f}% vs {summary['b_siege_3x3_rate_pct']:.2f}%"
    )
    print(f"Reasons: {summary['reasons']}")
    if wall_s is not None:
        print(f"Wall time: {wall_s:.1f}s")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run CLP-EP v1 engine-vs-engine match.")
    parser.add_argument("--run", required=True, help="Run name (used for games/<run>/ and EvE LPGF dir).")
    parser.add_argument("--engine-a", type=Path, required=True, help="Path to Engine A exe.")
    parser.add_argument("--engine-b", type=Path, required=True, help="Path to Engine B exe.")
    parser.add_argument("--name-a", default=None, help="Display name for Engine A.")
    parser.add_argument("--name-b", default=None, help="Display name for Engine B.")
    parser.add_argument("--env-a", default=None, help="Extra env for Engine A (KEY1=VAL1;KEY2=VAL2).")
    parser.add_argument("--env-b", default=None, help="Extra env for Engine B (KEY1=VAL1;KEY2=VAL2).")
    parser.add_argument("--games", type=int, default=400, help="Total games (even number preferred).")
    parser.add_argument("--movetime", type=int, default=150, help="Fixed movetime per move (ms).")
    parser.add_argument("--movetime-a", type=int, default=None, help="Override movetime for Engine A (ms).")
    parser.add_argument("--movetime-b", type=int, default=None, help="Override movetime for Engine B (ms).")
    parser.add_argument("--grace-ms", type=int, default=1000, help="Timeout grace beyond movetime (ms).")
    parser.add_argument("--max-ply", type=int, default=300, help="Draw at ply >= max-ply.")
    parser.add_argument("--openings", type=Path, default=HOMEI_ROOT / "openings.txt")
    parser.add_argument("--startpos", action="store_true", help="Play all games from standard initial position (no opening book/moves).")
    parser.add_argument(
        "--lpgf-root",
        type=Path,
        default=EVE_LPGF_ROOT,
        help="Root directory for exported LPGF v2.0 games.",
    )
    parser.add_argument(
        "--results-jsonl",
        type=Path,
        default=HOMEI_ROOT / "results.jsonl",
        help="Master JSONL file appending all completed games.",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=max_safe_workers(),
        help="Number of concurrent games (capped at min(4, cpu_count//2)).",
    )
    parser.add_argument("--stats-only", action="store_true", help="Only print stats for existing run.")
    parser.add_argument("--sprt", action="store_true", help="Enable pentanomial SPRT early stopping.")
    parser.add_argument("--elo0", type=float, default=0.0, help="SPRT null hypothesis Elo (default: 0.0).")
    parser.add_argument("--elo1", type=float, default=10.0, help="SPRT alt hypothesis Elo (default: 10.0).")
    parser.add_argument("--alpha", type=float, default=0.05, help="SPRT false positive rate (default: 0.05).")
    parser.add_argument("--beta", type=float, default=0.05, help="SPRT false negative rate (default: 0.05).")
    parser.add_argument("--sprt-min-pairs", type=int, default=10, help="Minimum complete pairs before SPRT stop.")
    parser.add_argument("--progress-interval", type=int, default=10, help="Print progress every N completed games.")
    args = parser.parse_args()

    check_free_disk(HOMEI_ROOT)
    workers = min(args.workers, max_safe_workers())

    run_dir = HOMEI_ROOT / "games" / args.run
    run_dir.mkdir(parents=True, exist_ok=True)
    run_jsonl = run_dir / "results.jsonl"

    lpgf_dir = args.lpgf_root / args.run
    lpgf_dir.mkdir(parents=True, exist_ok=True)

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
        summary = compute_stats(
            records,
            elo0=args.elo0,
            elo1=args.elo1,
            alpha=args.alpha,
            beta=args.beta,
            sprt_min_pairs=args.sprt_min_pairs,
        )
        print_stats(summary)
        return 0

    openings = [(STARTPOS_LPPN, [])] if args.startpos else load_openings(args.openings)
    name_a = args.name_a or args.engine_a.stem
    name_b = args.name_b or args.engine_b.stem
    env_a = parse_env_arg(args.env_a)
    env_b = parse_env_arg(args.env_b)
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
                "env_a": env_a,
                "env_b": env_b,
                "mt_a": mt_a,
                "mt_b": mt_b,
                "a_is_cam": a_is_cam,
                "max_ply": args.max_ply,
                "grace_ms": args.grace_ms,
                "games_dir": str(run_dir),
                "lpgf_dir": str(lpgf_dir),
            }
        )

    sprt_desc = (
        f" | SPRT[{args.elo0:+.1f},{args.elo1:+.1f}]" if args.sprt else ""
    )
    print(
        f"[match] Run={args.run} | {name_a} ({mt_a}ms) vs {name_b} ({mt_b}ms) | "
        f"target={args.games}, resumed={len(existing)}, pending={len(pending_tasks)}, workers={workers}{sprt_desc}"
    )

    t0 = time.perf_counter()
    sprt_stopped = False
    if pending_tasks:
        with HeavyLock(f"match_{args.run}"), HeartbeatMonitor(f"match_{args.run}") as hb:
            hb.update("playing", completed=len(existing), total=args.games)
            task_iter = iter(pending_tasks)
            max_in_flight = workers * 2 if args.sprt else len(pending_tasks)
            with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
                in_flight: Dict[concurrent.futures.Future, int] = {}
                for _ in range(min(max_in_flight, len(pending_tasks))):
                    try:
                        t = next(task_iter)
                        in_flight[pool.submit(play_single_game, t)] = t["game_id"]
                    except StopIteration:
                        break

                done_now = 0
                interval = max(1, args.progress_interval)
                while in_flight:
                    done_futs, _ = concurrent.futures.wait(
                        in_flight.keys(), return_when=concurrent.futures.FIRST_COMPLETED
                    )
                    for fut in done_futs:
                        in_flight.pop(fut)
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
                        hb.update("playing", completed=len(existing), total=args.games)

                        recs = [existing[k] for k in sorted(existing) if k < args.games]
                        s = compute_stats(
                            recs,
                            elo0=args.elo0,
                            elo1=args.elo1,
                            alpha=args.alpha,
                            beta=args.beta,
                            sprt_min_pairs=args.sprt_min_pairs,
                        )
                        sp = s["sprt"]
                        if args.sprt and sp["decision"] in ("H0", "H1"):
                            sprt_stopped = True
                        if done_now % interval == 0 or (not in_flight and sprt_stopped) or done_now == len(pending_tasks) or (args.sprt and sp["decision"] != "inconclusive"):
                            dt = time.perf_counter() - t0
                            pt = sp["pentanomial"]
                            print(
                                f"[progress {len(recs)}/{args.games}] W/D/L={s['wins_a']}/{s['draws']}/{s['losses_a']} "
                                f"Elo={s['elo']:+.1f}±{s['elo_ci95']:.1f} LOS={s['los_pct']:.1f}% "
                                f"Ptnml=[{pt['LL']},{pt['LD']},{pt['WL_DD']},{pt['WD']},{pt['WW']}] "
                                f"LLR={sp['llr']:+.2f} [{sp['la']:+.2f},{sp['lb']:+.2f}] {sp['decision']} "
                                f"({dt:.1f}s)",
                                flush=True,
                            )

                    if sprt_stopped:
                        for rem_fut in list(in_flight.keys()):
                            rem_fut.cancel()
                        # Wait cleanly for any already-started workers in flight to finish saving
                        continue
                    while len(in_flight) < max_in_flight:
                        try:
                            t = next(task_iter)
                            in_flight[pool.submit(play_single_game, t)] = t["game_id"]
                        except StopIteration:
                            break

    wall_s = time.perf_counter() - t0
    final_records = [existing[k] for k in sorted(existing) if k < args.games]
    summary = compute_stats(
        final_records,
        elo0=args.elo0,
        elo1=args.elo1,
        alpha=args.alpha,
        beta=args.beta,
        sprt_min_pairs=args.sprt_min_pairs,
    )
    print_stats(summary, wall_s=wall_s)
    (run_dir / "summary.json").write_text(
        json.dumps({**summary, "wall_s": round(wall_s, 2), "sprt_stopped": sprt_stopped}, indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

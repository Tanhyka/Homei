"""Step 2 Data Extraction Pipeline (tools/extract_koma_data.py).

Scans all 238 human games in E:\\code5\\CLP_dataset\\LPGF\\HvH\\ using ref.clp.record_io:
- Truncates before HISTORICAL_ILLEGAL_LOSS / illegal moves.
- Splits 80/20 train/val BY GAME before horizontal mirror augmentation.
- Deduplicates by canonical LPPN (including horizontal mirror deduplication across games).
- Filters ply 6..45 quiet positions: not in check, |static_eval - qsearch_score| <= 30 cp, |score| <= 1500.
- Evaluates with Homei_v3 at depth 5 (keeps m_human if within 60 cp of bestmove) and depth 6 (score_d6 MSE label).
- Generates up to 3 legal negative moves m_neg per position from Step 1 penalized categories.
- Uses qleaf + evalterms once per move to save leaf feature vectors into data/koma_features.jsonl.
"""

from __future__ import annotations

import concurrent.futures
import json
import os
from pathlib import Path
import random
import re
import sys
import time
from typing import Any, Dict, List, Optional, Set, Tuple

HOMEI_ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = HOMEI_ROOT / "HomeiEngine" / "tests"
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

from ref.clp import board as clp_board
from ref.clp import core_bridge
from ref.clp import lppn as clp_lppn
from ref.clp import pieces as clp_pieces
from ref.clp import record_io
from ref.clp import rules as clp_rules
from tools.safe_proc import HeartbeatMonitor, HeavyLock, SafeEngineClient, max_safe_workers

HVH_DIR = Path(r"E:\code5\CLP_dataset\LPGF\HvH")
V3_EXE = HOMEI_ROOT / "HomeiEngine" / "build" / "Engine realese" / "Homei_v3.exe"
NEW_EXE = HOMEI_ROOT / "HomeiEngine" / "build" / "homei.exe"
DATA_DIR = HOMEI_ROOT / "data"
OUT_JSONL = DATA_DIR / "koma_features.jsonl"
STATS_JSON = DATA_DIR / "koma_funnel_stats.json"


def _board_key(lppn_str: str) -> str:
    """Return LPPN without ply counter for strict position deduplication."""
    parts = lppn_str.strip().split()
    return " ".join(parts[:5]) if len(parts) >= 5 else lppn_str.strip()


def _mirror_board_and_turn(b: clp_board.Board, turn: clp_pieces.Player) -> clp_board.Board:
    """Horizontal mirror (col -> 8 - col, facing 1 <-> 3 for rotatable pieces)."""
    mb = clp_board.Board()
    mb.reserve = {
        clp_pieces.Player.CAM: list(b.reserve[clp_pieces.Player.CAM]),
        clp_pieces.Player.XANH: list(b.reserve[clp_pieces.Player.XANH]),
    }
    mb.prisoners = {
        clp_pieces.Player.CAM: list(b.prisoners[clp_pieces.Player.CAM]),
        clp_pieces.Player.XANH: list(b.prisoners[clp_pieces.Player.XANH]),
    }
    for r in range(9):
        for c in range(9):
            sq = b.sq(r, c)
            if sq.is_empty:
                continue
            msq = mb.sq(r, 8 - c)
            for p in sq.stack:
                nf = p.facing
                if p.ptype.name not in ("VUONG", "LONG", "PHUONG"):
                    if nf == 1:
                        nf = 3
                    elif nf == 3:
                        nf = 1
                msq.stack.append(clp_pieces.Piece(p.ptype, p.owner, nf))
    return mb


def _mirror_move(mv: clp_rules.Move, b_mir: clp_board.Board) -> clp_rules.Move:
    src_m = (mv.src[0], 8 - mv.src[1]) if mv.src is not None else None
    dst_m = (mv.dst[0], 8 - mv.dst[1]) if mv.dst is not None else None
    facing_m = mv.facing
    if facing_m is not None and mv.ptype and mv.ptype.name not in ("VUONG", "LONG", "PHUONG"):
        if facing_m == 1:
            facing_m = 3
        elif facing_m == 3:
            facing_m = 1
    rot_m = mv.rotate_after
    if rot_m in (1, 3):
        rot_m = 4 - rot_m
    return clp_rules.Move(
        kind=mv.kind,
        src=src_m,
        dst=dst_m,
        ptype=mv.ptype,
        facing=facing_m,
        rotate_after=rot_m,
    )


def _has_siege_ray_py(b: clp_board.Board, sq_rc: Tuple[int, int], slider_ptype: str, slider_facing: int, ek_rc: Optional[Tuple[int, int]]) -> bool:
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


def _find_king_rc(b: clp_board.Board, owner: clp_pieces.Player) -> Optional[Tuple[int, int]]:
    for r in range(9):
        for c in range(9):
            sq = b.sq(r, c)
            for p in sq.stack:
                if p.ptype == clp_pieces.PieceType.VUONG and p.owner == owner:
                    return (r, c)
    return None


def classify_koma_bad_move(b: clp_board.Board, turn: clp_pieces.Player, mv: clp_rules.Move) -> Optional[str]:
    """Return bad-move category string if `mv` is a quiet move in one of the Step 1 penalized categories."""
    if mv.kind == "drop":
        # Check dropping a rotatable piece directly facing into the board edge
        if mv.ptype and mv.ptype.name in ("BINH", "KI", "SI", "XA") and mv.dst is not None:
            r, c = mv.dst
            f = mv.facing or 0
            if (f == 0 and r == 0) or (f == 1 and c == 8) or (f == 2 and r == 8) or (f == 3 and c == 0):
                return "edge_dead_facing"
        return None

    # Must not be a capture move
    if mv.kind in ("single", "team") and mv.dst is not None:
        dst_cell = b.sq(*mv.dst)
        if not dst_cell.is_empty and dst_cell.top.owner != turn:
            return None

    if mv.src is None:
        return None
    src_cell = b.sq(*mv.src)
    if src_cell.is_empty:
        return None

    top_p = src_cell.top
    pt_name = top_p.ptype.name
    end_rc = mv.src if mv.kind == "rotate" else mv.dst
    if end_rc is None:
        return None
    r_end, c_end = end_rc
    rel_row_end = r_end if turn == clp_pieces.Player.CAM else (8 - r_end)
    rel_row_src = mv.src[0] if turn == clp_pieces.Player.CAM else (8 - mv.src[0])
    end_f = (top_p.facing + mv.rotate_after) % 4 if pt_name not in ("VUONG", "LONG", "PHUONG") else top_p.facing

    # Must not give check
    b_after = b.clone()
    clp_rules.apply_move(b_after, turn, mv)
    enemy = clp_rules.other(turn)
    if clp_rules.is_in_check(b_after, enemy):
        return None

    # Category 1: Burying Long/Phượng/Xa under Binh/Sĩ/Tướng in home half or without siege ray
    if mv.kind in ("single", "team") and mv.dst is not None and pt_name in ("BINH", "SI", "TUONG"):
        dst_cell = b.sq(*mv.dst)
        if not dst_cell.is_empty and dst_cell.top.owner == turn:
            under_p = dst_cell.top
            if under_p.ptype.name in ("LONG", "PHUONG", "XA"):
                ek_rc = _find_king_rc(b_after, enemy)
                has_ray = _has_siege_ray_py(b_after, mv.dst, under_p.ptype.name, under_p.facing, ek_rc)
                if rel_row_end <= 3 or not has_ray:
                    return "bury_slider"

    # Category 2: Rotating or moving Binh/Kị/Sĩ/Xa so its facing points directly into the board edge
    if pt_name in ("BINH", "KI", "SI", "XA"):
        points_out = (
            (end_f == 0 and r_end == 0)
            or (end_f == 1 and c_end == 8)
            or (end_f == 2 and r_end == 8)
            or (end_f == 3 and c_end == 0)
        )
        if points_out:
            return "edge_dead_facing"
        if pt_name == "KI":
            # Knight jump requires 2 rows in facing direction
            ki_out = (
                (end_f == 0 and r_end <= 1)
                or (end_f == 1 and c_end >= 7)
                or (end_f == 2 and r_end >= 7)
                or (end_f == 3 and c_end <= 1)
            )
            if ki_out:
                return "edge_dead_facing"

    # Category 3: Retreating Sĩ/Kị back toward starting rank (rel_row_end == 0 or backward step in home half)
    if mv.kind in ("single", "team") and pt_name in ("SI", "KI"):
        if rel_row_end < rel_row_src and rel_row_end <= 1:
            return "retreat_si_ki_backrank"

    # Category 4: In-place rotation of Binh in home territory (rel_row <= 3)
    if mv.kind == "rotate" and pt_name == "BINH" and rel_row_end <= 3:
        backward_dir = 0 if turn == clp_pieces.Player.CAM else 2
        if end_f == backward_dir:
            return "home_binh_rotate_back"
        return "home_binh_rotate"

    return None


def select_negative_moves(
    b: clp_board.Board, turn: clp_pieces.Player, legal_moves: List[clp_rules.Move], max_neg: int = 3
) -> List[Tuple[clp_rules.Move, str, str]]:
    """Select up to `max_neg` negative moves (move_obj, canonical_notation, category), prioritizing diverse categories."""
    by_priority: List[Tuple[int, str, clp_rules.Move, str]] = []
    priority_map = {
        "bury_slider": 0,
        "edge_dead_facing": 1,
        "retreat_si_ki_backrank": 2,
        "home_binh_rotate_back": 3,
        "home_binh_rotate": 4,
    }
    for mv in legal_moves:
        cat = classify_koma_bad_move(b, turn, mv)
        if cat is not None:
            canon = mv.canonical_notation(b)
            by_priority.append((priority_map.get(cat, 9), canon, mv, cat))

    by_priority.sort(key=lambda x: (x[0], x[1]))
    chosen: List[Tuple[clp_rules.Move, str, str]] = []
    used_cats: Set[str] = set()
    # Pass 1: pick at most 1 from each distinct category first
    for _, canon, mv, cat in by_priority:
        if len(chosen) >= max_neg:
            break
        if cat not in used_cats:
            used_cats.add(cat)
            chosen.append((mv, canon, cat))
    # Pass 2: fill remaining slots up to max_neg
    for _, canon, mv, cat in by_priority:
        if len(chosen) >= max_neg:
            break
        if all(c != canon for _, c, _ in chosen):
            chosen.append((mv, canon, cat))
    return chosen


def _parse_evalterms_from_client(client: SafeEngineClient, lppn_str: str) -> Tuple[int, List[int]]:
    client.send_line(f"evalterms {lppn_str}")
    hdr = client.read_line()
    if hdr != "evalterms begin":
        raise RuntimeError(f"Expected 'evalterms begin', got {hdr!r}")
    base_fixed = 0
    raw_list: List[int] = []
    while True:
        line = client.read_line()
        if line == "evalterms end":
            break
        if line.startswith("term "):
            parts = line.split()
            name = parts[1].split("=", 1)[1]
            raw_v = int(parts[2].split("=", 1)[1])
            if name == "base_fixed":
                base_fixed = raw_v
            else:
                raw_list.append(raw_v)
    return base_fixed, raw_list


def _query_qleaf_features(client: SafeEngineClient, root_lppn: str, move_not: str, root_turn_char: str) -> Tuple[int, int, List[int], str]:
    """Run qleaf <root_lppn> <move_not>, then evalterms on leaf LPPN, oriented to root mover perspective."""
    client.send_line(f"qleaf {root_lppn} {move_not}")
    resp = client.read_line()
    if not resp.startswith("qleaf "):
        raise RuntimeError(f"qleaf failed for {move_not} on {root_lppn}: {resp!r}")
    parts = resp.split(" ", 3)
    mover_qscore = int(parts[1])
    qply_len = int(parts[2])
    leaf_lppn = parts[3]

    leaf_turn_char = leaf_lppn.split()[2]
    sign = 1 if leaf_turn_char == root_turn_char else -1
    base_fixed, raw_vec = _parse_evalterms_from_client(client, leaf_lppn)
    return mover_qscore, sign * base_fixed, [sign * r for r in raw_vec], leaf_lppn


def _worker_evaluate_chunk(tasks: List[dict]) -> List[dict]:
    """Worker process: filters candidates via qcheck, evaluates depth 5/6 with Homei_v3, and extracts leaf features."""
    results: List[dict] = []
    with SafeEngineClient(V3_EXE) as c_v3, SafeEngineClient(
        NEW_EXE, extra_env={"HOMEI_FEATURES": "+koma_eval,-koma_search", "HOMEI_PARAMS": ""}
    ) as c_koma:
        for item in tasks:
            lppn_str = item["lppn"]
            mir_lppn_str = item["mir_lppn"]
            m_human = item["m_human"]
            mir_m_human = item["mir_m_human"]
            negs = item["negs"]
            mir_negs = item["mir_negs"]

            # 1. Check quietness via qcheck on c_koma (Wait: use Homei_v3 eval via c_koma with koma_eval=0?
            # Note: in_check is identical; let's check both static_eval and qsearch)
            c_koma.send_line(f"qcheck {lppn_str}")
            qline = c_koma.read_line()
            if not qline.startswith("qcheck "):
                results.append({"status": "error"})
                continue
            qparts = qline.split(" ", 5)
            in_chk = int(qparts[1])
            static_ev = int(qparts[2])
            qscore = int(qparts[3])

            if in_chk != 0:
                results.append({"status": "in_check"})
                continue
            if abs(static_ev - qscore) > 30:
                results.append({"status": "non_quiet"})
                continue
            if abs(static_ev) > 1500 or abs(qscore) > 1500:
                results.append({"status": "extreme_score"})
                continue

            # 2. Search with Homei_v3 up to depth 6
            out_d6 = c_v3.search_fixed_depth(lppn_str, depth=6, multipv=1, movetime_ms=1500)
            if out_d6.status != "ok" or not out_d6.infos:
                results.append({"status": "error"})
                continue

            mpv1 = [inf for inf in out_d6.infos if inf.get("multipv", 1) == 1]
            if not mpv1:
                results.append({"status": "error"})
                continue

            info_d5 = next((inf for inf in reversed(mpv1) if int(inf.get("depth", 0)) == 5), mpv1[-1])
            info_d6 = mpv1[-1]

            sc_d5_obj = info_d5.get("score", {})
            sc_d6_obj = info_d6.get("score", {})
            if sc_d5_obj.get("kind") != "cp" or sc_d6_obj.get("kind") != "cp":
                results.append({"status": "mate_score"})
                continue

            best_score_d5 = int(sc_d5_obj.get("value", 0))
            score_d6 = int(sc_d6_obj.get("value", 0))
            if abs(best_score_d5) > 1500 or abs(score_d6) > 1500:
                results.append({"status": "extreme_score"})
                continue

            pv_d5 = info_d5.get("pv", [])
            bestmove_d5 = pv_d5[0]["notation"] if pv_d5 else (out_d6.bestmove["notation"] if out_d6.bestmove else "")

            # Check human move score at depth 5
            if m_human == bestmove_d5:
                human_score_d5 = best_score_d5
            else:
                after_human_lppn = item["after_human_lppn"]
                out_h4 = c_v3.search_fixed_depth(after_human_lppn, depth=4, multipv=1, movetime_ms=800)
                if out_h4.status != "ok" or not out_h4.infos:
                    results.append({"status": "error"})
                    continue
                h_mpv1 = [inf for inf in out_h4.infos if inf.get("multipv", 1) == 1]
                h_sc = h_mpv1[-1].get("score", {})
                if h_sc.get("kind") != "cp":
                    results.append({"status": "mate_score"})
                    continue
                human_score_d5 = -int(h_sc.get("value", 0))

            if best_score_d5 - human_score_d5 > 60:
                results.append({"status": "human_blunder"})
                continue

            # 3. Extract features for original and horizontally mirrored position
            records_pair = []
            for is_mir, cur_lppn, cur_m_pos, cur_negs in (
                (False, lppn_str, m_human, negs),
                (True, mir_lppn_str, mir_m_human, mir_negs),
            ):
                root_turn_char = cur_lppn.split()[2]
                root_base, root_raw = _parse_evalterms_from_client(c_koma, cur_lppn)
                _, pos_base, pos_raw, pos_leaf_lppn = _query_qleaf_features(
                    c_koma, cur_lppn, cur_m_pos, root_turn_char
                )
                neg_entries = []
                for neg_not, neg_cat in cur_negs:
                    _, neg_base, neg_raw, neg_leaf_lppn = _query_qleaf_features(
                        c_koma, cur_lppn, neg_not, root_turn_char
                    )
                    neg_entries.append(
                        {
                            "move": neg_not,
                            "category": neg_cat,
                            "leaf_lppn": neg_leaf_lppn,
                            "base_fixed": neg_base,
                            "raw": neg_raw,
                        }
                    )
                records_pair.append(
                    {
                        "game_file": item["game_file"],
                        "split": item["split"],
                        "is_mirror": is_mir,
                        "ply": item["ply"],
                        "root_lppn": cur_lppn,
                        "score_d6": score_d6,
                        "best_score_d5": best_score_d5,
                        "human_score_d5": human_score_d5,
                        "root_base_fixed": root_base,
                        "root_raw": root_raw,
                        "m_pos": {
                            "move": cur_m_pos,
                            "leaf_lppn": pos_leaf_lppn,
                            "base_fixed": pos_base,
                            "raw": pos_raw,
                        },
                        "m_negs": neg_entries,
                    }
                )

            results.append({"status": "accepted", "records": records_pair})
    return results


def run_extraction() -> dict:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    lpgf_files = sorted(HVH_DIR.glob("*.lpgf"))
    total_files = len(lpgf_files)
    if total_files != 238:
        print(f"[WARN] Expected 238 .lpgf files in {HVH_DIR}, found {total_files}")

    # Deterministic 80/20 split by game
    rng = random.Random(12345)
    indices = list(range(total_files))
    rng.shuffle(indices)
    n_train_games = int(round(0.80 * total_files))
    train_idx_set = set(indices[:n_train_games])

    funnel = {
        "files_scanned": total_files,
        "valid_games": 0,
        "train_games": 0,
        "val_games": 0,
        "historical_illegal_truncated_games": 0,
        "total_plies_replayed": 0,
        "ply_6_to_45_raw": 0,
        "dedup_unique_positions": 0,
        "rejected_human_bad_koma": 0,
        "rejected_no_neg_moves": 0,
        "candidate_for_engine": 0,
        "rejected_in_check": 0,
        "rejected_non_quiet": 0,
        "rejected_extreme_or_mate": 0,
        "rejected_human_gt_60cp": 0,
        "accepted_base_positions": 0,
        "accepted_total_positions_with_mirror": 0,
        "train_positions": 0,
        "val_positions": 0,
        "train_pairs": 0,
        "val_pairs": 0,
        "total_pairs": 0,
        "neg_category_counts": {},
    }

    seen_canonical_keys: Set[str] = set()
    candidates: List[dict] = []

    t0 = time.perf_counter()
    print(f"[extract] Scanning {total_files} LPGF files in {HVH_DIR}...")

    for file_idx, fpath in enumerate(lpgf_files):
        split = "train" if file_idx in train_idx_set else "val"
        raw_text = fpath.read_text(encoding="utf-8", errors="replace")
        has_hist_illegal_tag = "HISTORICAL_ILLEGAL_LOSS" in raw_text

        rec = record_io.ImportedRecord()
        if not record_io.RecordIO.read(str(fpath), rec):
            continue

        bridge = core_bridge.CoreBridge()
        app_plies, tot_tokens, err_str = record_io.RecordIO.load_txt_moves_into_bridge(bridge, rec)
        if has_hist_illegal_tag or (err_str is not None and app_plies > 0):
            funnel["historical_illegal_truncated_games"] += 1

        if app_plies <= 0:
            continue

        funnel["valid_games"] += 1
        if split == "train":
            funnel["train_games"] += 1
        else:
            funnel["val_games"] += 1
        funnel["total_plies_replayed"] += app_plies

        nodes = bridge._active_line_nodes()
        for i in range(len(nodes) - 1):
            node = nodes[i]
            next_node = nodes[i + 1]
            ply = int(node.ply)
            if not (6 <= ply <= 45):
                continue
            if not next_node.move:
                continue

            funnel["ply_6_to_45_raw"] += 1
            b, turn = core_bridge._decode_position_py(node.state["position"])
            lppn_str = clp_lppn.to_lppn(b, turn, ply=ply, strict=True)
            b_mir = _mirror_board_and_turn(b, turn)
            mir_lppn_str = clp_lppn.to_lppn(b_mir, turn, ply=ply, strict=True)

            k1 = _board_key(lppn_str)
            k2 = _board_key(mir_lppn_str)
            canon_pair_key = min(k1, k2)
            if canon_pair_key in seen_canonical_keys:
                continue
            seen_canonical_keys.add(canon_pair_key)
            funnel["dedup_unique_positions"] += 1

            # Match human move on board
            md = next_node.move
            legal = clp_rules.legal_moves(b, turn)
            matched = [
                m
                for m in legal
                if m.kind == md["kind"]
                and (list(m.src) if m.src is not None else None) == md["src"]
                and (list(m.dst) if m.dst is not None else None) == md["dst"]
                and (m.ptype.name if m.ptype is not None else "") == md["ptype"]
                and m.facing == md["facing"]
                and m.rotate_after == md["rotate_after"]
            ]
            if len(matched) != 1:
                continue
            mv_human = matched[0]

            # Ensure m_human is not itself a bad koma move
            if classify_koma_bad_move(b, turn, mv_human) is not None:
                funnel["rejected_human_bad_koma"] += 1
                continue

            negs_full = select_negative_moves(b, turn, legal, max_neg=3)
            if not negs_full:
                funnel["rejected_no_neg_moves"] += 1
                continue

            # Build mirrored moves directly from selected moves to guarantee exact symmetry
            mv_human_mir = _mirror_move(mv_human, b_mir)
            negs = [(canon, cat) for _, canon, cat in negs_full]
            mir_negs = [(_mirror_move(mv_neg, b_mir).canonical_notation(b_mir), cat) for mv_neg, _, cat in negs_full]

            b_after = b.clone()
            clp_rules.apply_move(b_after, turn, mv_human)
            after_human_lppn = clp_lppn.to_lppn(b_after, clp_rules.other(turn), ply=ply + 1, strict=True)

            funnel["candidate_for_engine"] += 1
            candidates.append(
                {
                    "game_file": fpath.name,
                    "split": split,
                    "ply": ply,
                    "lppn": lppn_str,
                    "mir_lppn": mir_lppn_str,
                    "m_human": mv_human.canonical_notation(b),
                    "mir_m_human": mv_human_mir.canonical_notation(b_mir),
                    "after_human_lppn": after_human_lppn,
                    "negs": negs,
                    "mir_negs": mir_negs,
                }
            )

    print(
        f"[extract] Parsed {funnel['valid_games']}/{total_files} games "
        f"({funnel['total_plies_replayed']} plies) -> {len(candidates)} unique candidates for engine filtering."
    )

    workers = max_safe_workers()
    chunk_size = 25
    chunks = [candidates[i : i + chunk_size] for i in range(0, len(candidates), chunk_size)]

    all_records: List[dict] = []
    with HeavyLock("extract_koma_data"), HeartbeatMonitor("extract_koma_data") as hb:
        hb.update("engine_filtering", completed=0, total=len(candidates))
        done_candidates = 0
        with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(_worker_evaluate_chunk, ch) for ch in chunks]
            for fut in concurrent.futures.as_completed(futures):
                chunk_res = fut.result()
                for r in chunk_res:
                    done_candidates += 1
                    st = r["status"]
                    if st == "in_check":
                        funnel["rejected_in_check"] += 1
                    elif st == "non_quiet":
                        funnel["rejected_non_quiet"] += 1
                    elif st in ("extreme_score", "mate_score"):
                        funnel["rejected_extreme_or_mate"] += 1
                    elif st == "human_blunder":
                        funnel["rejected_human_gt_60cp"] += 1
                    elif st == "accepted":
                        funnel["accepted_base_positions"] += 1
                        for rec_item in r["records"]:
                            all_records.append(rec_item)
                            funnel["accepted_total_positions_with_mirror"] += 1
                            n_neg = len(rec_item["m_negs"])
                            if rec_item["split"] == "train":
                                funnel["train_positions"] += 1
                                funnel["train_pairs"] += n_neg
                            else:
                                funnel["val_positions"] += 1
                                funnel["val_pairs"] += n_neg
                            funnel["total_pairs"] += n_neg
                            for ng in rec_item["m_negs"]:
                                c_name = ng["category"]
                                funnel["neg_category_counts"][c_name] = (
                                    funnel["neg_category_counts"].get(c_name, 0) + 1
                                )
                hb.update("engine_filtering", completed=done_candidates, total=len(candidates))
                if done_candidates % 200 == 0 or done_candidates == len(candidates):
                    print(
                        f"[extract] Progress {done_candidates}/{len(candidates)} | "
                        f"accepted_pos={funnel['accepted_total_positions_with_mirror']} "
                        f"pairs={funnel['total_pairs']} ({time.perf_counter() - t0:.1f}s)"
                    )

    # Sort deterministically for reproducibility
    all_records.sort(key=lambda x: (x["split"], x["game_file"], x["ply"], x["is_mirror"]))
    with OUT_JSONL.open("w", encoding="utf-8") as f:
        for rec_item in all_records:
            f.write(json.dumps(rec_item, ensure_ascii=False) + "\n")

    funnel["elapsed_sec"] = round(time.perf_counter() - t0, 2)
    STATS_JSON.write_text(json.dumps(funnel, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(funnel, indent=2, ensure_ascii=False))
    return funnel


if __name__ == "__main__":
    run_extraction()

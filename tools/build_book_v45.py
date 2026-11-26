"""Step 6b Opening Book Builder for Homei v4-5 (Tiền trận 4-ply + Early Middlegame ply 4..16).

Two-pass deep verification powered by Homei_v4-4.exe (~900k NPS):
  - Pass A (Plies 0..2): Complete canonical Tiền trận matrix (ply 0 startpos, all 10 canonical
    ply-1 drops covering all 18 CAM moves via mirror, plus all 18 XANH replies against the
    top CAM drops -> full coverage against any opponent ply-1 reply!).
  - Pass B (Plies 3..16): Expands Pass A's verified ply-2 replies into ply-3 AND mines
    all frequent early middlegame positions (plies 3..16) from Arena 4 (v4-4 vs v4-3 @ 2000ms/
    500ms/150ms + round-robin) and HvH_v2_draft human games.
  - Every kept canonical position is verified by Homei_v4-4.exe (OwnBook=0, MultiPV=3..5),
    strictly filtered for blunders, expanded with exact horizontal mirrors, and queried for
    exact v4-4 Split Zobrist hashes directly from the engine.

Outputs (all synchronized):
  1. HomeiEngine/src/book_data.h          (embedded sorted C++ BookEntry[])
  2. Homei/books/homei_v45_book.clpbook   (fast external text book for BookFile option)
  3. ColongphuongGUI_2/books/homei_v45_book.json (GUI BookPanel JSON with W/D/L & cp comments)
"""

from __future__ import annotations

import collections
import concurrent.futures
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

HOMEI_ROOT = Path(r"E:\all-code\Code1\CLP_engine\Homei")
GUI_ROOT = Path(r"E:\all-code\Code1\ColongphuongGUI_2")
DATASET_ROOT = Path(r"E:\all-code\Code1\CLP_dataset\LPGF")

sys.path.insert(0, str(GUI_ROOT))
sys.path.insert(0, str(HOMEI_ROOT / "HomeiEngine" / "tests"))

from engine.board import Board
from engine.game import Game
from engine.lppn import from_lppn, to_lppn
from engine.notation import LPGFNotation
from engine.pieces import Piece, Player
from engine import rules
from clp_ep_client import CLPEPClient, build_state_from_lppn

ENGINE_EXE = HOMEI_ROOT / "Engine release" / "Homei_v4-4.exe"
BOOK_DATA_H = HOMEI_ROOT / "HomeiEngine" / "src" / "book_data.h"
CLPBOOK_OUT = HOMEI_ROOT / "books" / "homei_v45_book.clpbook"
GUI_JSON_OUT = GUI_ROOT / "books" / "homei_v45_book.json"

START_LPPN = "lppn1 X2K2S2T2V2T2S2K2X2/9/B2B2B2B2B2B2B2B2B2/9/9/9/b0b0b0b0b0b0b0b0b0/9/x0k0s0t0v0t0s0k0x0 c LP/LP -/- 0"

MOVE_KIND_MAP = {"single": 0, "team": 1, "rotate": 2, "drop": 3}
PTYPE_MAP = {"VUONG": 0, "TUONG": 1, "SI": 2, "KI": 3, "XA": 4, "LONG": 5, "PHUONG": 6, "BINH": 7}

MAX_BOOK_PLY = 16
WORKERS = 6


def lppn5(lp: str) -> str:
    parts = lp.strip().split()
    return " ".join(parts[:5]) if len(parts) >= 5 else lp.strip()


def mirror_facing(f: int) -> int:
    if f == 1:
        return 3
    if f == 3:
        return 1
    return f


def mirror_board_h(board: Board) -> Board:
    mb = board.clone()
    for r in range(9):
        for c in range(9):
            src_cell = board.sq(r, 8 - c)
            dst_cell = mb.sq(r, c)
            dst_cell.stack.clear()
            for p in src_cell.stack:
                dst_cell.stack.append(Piece(p.ptype, p.owner, mirror_facing(p.facing)))
    return mb


def mirror_move_dict(m: dict) -> dict:
    src = [m["src"][0], 8 - m["src"][1]] if m["src"] is not None else None
    dst = [m["dst"][0], 8 - m["dst"][1]] if m["dst"] is not None else None
    f = mirror_facing(int(m["facing"])) if m["facing"] is not None else None
    rot = {0: 0, 1: 3, 2: 2, 3: 1}.get(int(m["rotate_after"]) % 4, 0)
    return {
        "kind": m["kind"],
        "src": src,
        "dst": dst,
        "ptype": m["ptype"],
        "facing": f,
        "rotate_after": rot,
    }


def mirror_move_obj(mv: rules.Move) -> rules.Move:
    msrc = (mv.src[0], 8 - mv.src[1]) if mv.src is not None else None
    mdst = (mv.dst[0], 8 - mv.dst[1]) if mv.dst is not None else None
    mf = mirror_facing(mv.facing) if mv.facing is not None else None
    mrot = {0: 0, 1: 3, 2: 2, 3: 1}.get(mv.rotate_after % 4, 0)
    return rules.Move(mv.kind, msrc, mdst, mv.ptype, mf, mrot)


def canonical_lppn(board: Board, turn: Player, ply: int) -> Tuple[str, str, bool]:
    """Return (canon_lppn5, canon_full_lppn, was_mirrored)."""
    lp = to_lppn(board, turn, ply=ply)
    k1 = lppn5(lp)
    mb = mirror_board_h(board)
    mlp = to_lppn(mb, turn, ply=ply)
    k2 = lppn5(mlp)
    if k2 < k1:
        return k2, mlp, True
    return k1, lp, False


def pack_move_dict(m: dict) -> int:
    kind = MOVE_KIND_MAP[m["kind"]]
    src = (m["src"][0] * 9 + m["src"][1]) if m["src"] is not None else 255
    dst = (m["dst"][0] * 9 + m["dst"][1]) if m["dst"] is not None else 255
    ptype = PTYPE_MAP[m["ptype"]]
    facing_enc = (int(m["facing"]) + 1) if m["facing"] is not None else 0
    rot = int(m["rotate_after"]) & 3
    return (
        ((kind & 3) << 24)
        | ((src & 0xFF) << 16)
        | ((dst & 0xFF) << 8)
        | ((ptype & 7) << 5)
        | ((facing_enc & 7) << 2)
        | rot
    )


def _truncate_lpgf_text(txt: str, max_move_no: int = 18) -> str:
    """Keep header + first `max_move_no` move lines for ultra-fast opening mining."""
    out = []
    cutoff_pat = re.compile(rf"^\s*{max_move_no + 1}\.")
    for line in txt.splitlines():
        if cutoff_pat.match(line):
            break
        out.append(line)
    return "\n".join(out)


def _extract_winner(meta: dict) -> str:
    w = str(meta.get("winner") or "").lower()
    if "cam" in w and "xanh" not in w:
        return "CAM"
    if "xanh" in w and "cam" not in w:
        return "XANH"
    if "(cam" in w:
        return "CAM"
    if "(xanh" in w:
        return "XANH"
    if "hòa" in w or "hoa" in w or "draw" in w:
        return "DRAW"
    return ""


def mine_corpus() -> Tuple[Dict[str, str], Dict[str, float], Dict[Tuple[str, str], List[int]]]:
    """Mine opening positions (plies 0..16) and empirical W/D/L from Arena 4 + HvH."""
    sources: List[Tuple[Path, float]] = [
        (DATASET_ROOT / "EvE" / "Arena 4" / "arena4_v44_vs_v43_2000ms", 4.0),
        (DATASET_ROOT / "EvE" / "Arena 4" / "arena4_v44_vs_v43_500ms", 3.0),
        (DATASET_ROOT / "EvE" / "Arena 4" / "step5_v44_vs_v43", 2.5),
        (DATASET_ROOT / "EvE" / "Arena 4" / "arena4_v44_vs_v43_150ms", 2.0),
        (DATASET_ROOT / "EvE" / "Arena 4" / "step4_v43_vs_v42_clean", 1.5),
        (DATASET_ROOT / "EvE" / "Arena 4" / "rr_v43_vs_v42", 1.2),
        (DATASET_ROOT / "EvE" / "Arena 4" / "rr_v43_vs_v41", 1.0),
        (DATASET_ROOT / "HvH_v2_draft", 1.8),
    ]

    canon_lppn_map: Dict[str, str] = {}
    pos_weight: Dict[str, float] = collections.defaultdict(float)
    # (lppn5_exact, move_not_exact) -> [win, draw, loss] from side-to-move perspective
    wdl_stats: Dict[Tuple[str, str], List[int]] = collections.defaultdict(lambda: [0, 0, 0])

    total_games = 0
    t0 = time.perf_counter()

    for folder, src_w in sources:
        if not folder.is_dir():
            continue
        files = sorted(folder.glob("*.lpgf"))
        parsed_here = 0
        for fp in files:
            try:
                raw = fp.read_text(encoding="utf-8-sig", errors="ignore")
                if "HISTORICAL_ILLEGAL_LOSS" in raw:
                    continue
                short_txt = _truncate_lpgf_text(raw, MAX_BOOK_PLY + 2)
                g = Game()
                meta = LPGFNotation.load_record(short_txt, g, fp.name, truncate_on_illegal=True)
            except Exception:
                continue

            winner = _extract_winner(meta)
            # Walk main line nodes
            nodes = []
            nd = g.root
            while nd is not None:
                nodes.append(nd)
                nd = nd.children[0] if nd.children else None
            if len(nodes) < 5:
                continue
            # Only startpos games (ply 0 has LP/LP reserve)
            root_lp = to_lppn(nodes[0]._snapshot_board, nodes[0]._snapshot_current, ply=0)
            if "LP/LP" not in root_lp:
                continue

            parsed_here += 1
            total_games += 1
            for ply in range(min(MAX_BOOK_PLY, len(nodes) - 1)):
                cur_nd = nodes[ply]
                nxt_nd = nodes[ply + 1]
                b = cur_nd._snapshot_board
                turn = cur_nd._snapshot_current
                mv = nxt_nd.move
                if b is None or turn is None or mv is None:
                    break

                ck, c_lp, was_mir = canonical_lppn(b, turn, ply)
                canon_lppn_map.setdefault(ck, c_lp)
                # Prefer lower ply if transposed
                if int(c_lp.split()[-1]) < int(canon_lppn_map[ck].split()[-1]):
                    canon_lppn_map[ck] = c_lp
                pos_weight[ck] += src_w

                # Record W/D/L for BOTH exact board and horizontal mirror
                exact_k = lppn5(to_lppn(b, turn, ply=ply))
                mv_not = mv.notation(b)
                mb = mirror_board_h(b)
                mir_k = lppn5(to_lppn(mb, turn, ply=ply))
                mmv = mirror_move_obj(mv)
                mmv_not = mmv.notation(mb)

                if winner == turn.name:
                    slot = 0
                elif winner == "DRAW":
                    slot = 1
                elif winner in ("CAM", "XANH"):
                    slot = 2
                else:
                    slot = -1
                if slot >= 0:
                    wdl_stats[(exact_k, mv_not)][slot] += 1
                    if mir_k != exact_k:
                        wdl_stats[(mir_k, mmv_not)][slot] += 1

        print(f"  [mine] {folder.name}: {parsed_here}/{len(files)} startpos games parsed", flush=True)

    print(
        f"[mine] Done in {time.perf_counter() - t0:.1f}s: {total_games} games -> "
        f"{len(canon_lppn_map)} unique canonical positions (plies 0..{MAX_BOOK_PLY})",
        flush=True,
    )
    return canon_lppn_map, pos_weight, wdl_stats


def _build_pass_a_positions() -> List[Tuple[str, int, int]]:
    """Return list of (lppn, movetime_ms, multipv) for Pass A: plies 0, 1, and full ply-2 matrix."""
    tasks: Dict[str, Tuple[str, int, int]] = {}

    def add_pos(b: Board, turn: Player, ply: int, mt: int, mpv: int):
        ck, clp, _ = canonical_lppn(b, turn, ply)
        prev = tasks.get(ck)
        if prev is None or mt > prev[1]:
            tasks[ck] = (clp, mt, mpv)

    b0 = Board()
    b0.setup_initial()
    # Ply 0: startpos
    add_pos(b0, Player.CAM, 0, 5000, 5)

    # Ply 1: ALL 18 CAM drops -> 10 canonical positions (covers 100% of ply 1!)
    legal0 = rules.legal_moves(b0, Player.CAM)
    for m0 in legal0:
        b1 = b0.clone()
        rules.apply_move(b1, Player.CAM, m0)
        add_pos(b1, Player.XANH, 1, 2200, 5)

    # Ply 2: Top 8 canonical CAM ply-0 drops x ALL 18 XANH ply-1 responses (~144 canonical positions)
    # Covers L*b2..b5 and P*b2..b5 (via mirror: b2..b8 = every sensible CAM first drop!)
    seed_m0_nots = {"L*b5", "L*b4", "L*b3", "L*b2", "P*b5", "P*b4", "P*b3", "P*b2"}
    for m0 in legal0:
        if m0.notation(b0) not in seed_m0_nots:
            continue
        b1 = b0.clone()
        rules.apply_move(b1, Player.CAM, m0)
        legal1 = rules.legal_moves(b1, Player.XANH)
        for m1 in legal1:
            b2 = b1.clone()
            rules.apply_move(b2, Player.XANH, m1)
            add_pos(b2, Player.CAM, 2, 900, 3)

    out = list(tasks.values())
    out.sort(key=lambda x: int(x[0].split()[-1]))
    return out


def _eval_worker(chunk: List[Tuple[str, int, int]]) -> List[dict]:
    """Worker: run Homei_v4-4.exe (OwnBook=0) on each (lppn, movetime_ms, multipv) position."""
    results: List[dict] = []
    with CLPEPClient(ENGINE_EXE) as client:
        client.set_option("OwnBook", 0)
        client.set_option("Hash", 64)
        client.set_option("Threads", 1)
        for lp, mt, mpv in chunk:
            ply = int(lp.split()[-1])
            try:
                st, b_ref, turn_ref = build_state_from_lppn(lp)
            except Exception:
                continue
            client.set_position_lppn(lp)
            client.send_line("hash")
            hline = client.read_line()
            key = int(hline.split()[1])

            out = client.search_movetime(st, movetime_ms=mt, depth=14, nodes=50000000, multipv=mpv)
            if not out.infos:
                continue
            max_d = max(i["depth"] for i in out.infos)
            if max_d < 5:
                continue
            top_infos = [i for i in out.infos if i["depth"] == max_d]
            top_infos.sort(key=lambda i: i.get("multipv", 1))
            if not top_infos or top_infos[0]["score"]["kind"] != "cp":
                continue

            best_sc = int(top_infos[0]["score"]["value"])
            tol = 14 if ply <= 1 else (10 if ply <= 4 else 8)
            chosen: List[dict] = []
            for idx, inf in enumerate(top_infos):
                sc_info = inf["score"]
                if sc_info["kind"] != "cp" or not inf.get("pv"):
                    continue
                sc = int(sc_info["value"])
                mv = inf["pv"][0]
                if idx > 0 and sc < best_sc - tol:
                    continue
                diff = max(0, best_sc - sc)
                w = max(15, int(100 - diff * 5))
                if idx == 0:
                    w = int(w * 1.35)
                chosen.append({
                    "mv": mv,
                    "packed": pack_move_dict(mv),
                    "notation": mv["notation"],
                    "score": sc,
                    "weight": w,
                    "depth": max_d,
                })
            if not chosen:
                continue

            # Exact horizontal mirror hash + mirrored moves verified via engine!
            pos_dec = from_lppn(lp)
            mb = mirror_board_h(pos_dec.board)
            mlp = to_lppn(mb, pos_dec.turn, ply=ply)
            client.set_position_lppn(mlp)
            client.send_line("hash")
            mkey = int(client.read_line().split()[1])

            mir_chosen: List[dict] = []
            if mkey != key:
                mlegal = rules.legal_moves(mb, pos_dec.turn)
                # Map (kind, src, dst, ptype, facing, rotate_after) -> canonical notation on mb
                mlegal_map = {
                    (lm.kind, lm.src, lm.dst, lm.ptype.name, lm.facing, lm.rotate_after): lm.notation(mb)
                    for lm in mlegal
                }
                for ch in chosen:
                    mm = mirror_move_dict(ch["mv"])
                    sig = (
                        mm["kind"],
                        tuple(mm["src"]) if mm["src"] is not None else None,
                        tuple(mm["dst"]) if mm["dst"] is not None else None,
                        mm["ptype"],
                        mm["facing"],
                        mm["rotate_after"],
                    )
                    mn = mlegal_map.get(sig)
                    if mn:
                        mir_chosen.append({
                            "packed": pack_move_dict(mm),
                            "notation": mn,
                            "score": ch["score"],
                            "weight": ch["weight"],
                            "depth": ch["depth"],
                        })

            results.append({
                "lppn": lp,
                "lppn5": lppn5(lp),
                "ply": ply,
                "key": key,
                "chosen": chosen,
                "mir_lppn5": lppn5(mlp),
                "mkey": mkey,
                "mir_chosen": mir_chosen,
            })
    return results


def run_parallel_eval(tasks: List[Tuple[str, int, int]], label: str) -> List[dict]:
    t0 = time.perf_counter()
    print(f"[{label}] Deep-searching {len(tasks)} canonical positions with {ENGINE_EXE.name} ({WORKERS} workers)...", flush=True)
    chunks = [tasks[i::WORKERS] for i in range(WORKERS)]
    chunks = [c for c in chunks if c]
    all_res: List[dict] = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=len(chunks)) as pool:
        futs = [pool.submit(_eval_worker, c) for c in chunks]
        for fut in concurrent.futures.as_completed(futs):
            part = fut.result()
            all_res.extend(part)
            print(f"  [{label}] worker finished +{len(part)} positions (total {len(all_res)}/{len(tasks)}, {time.perf_counter() - t0:.1f}s)", flush=True)
    print(f"[{label}] Completed {len(all_res)} positions in {time.perf_counter() - t0:.1f}s", flush=True)
    return all_res


def build_pass_b_positions(
    pass_a_results: List[dict],
    corpus_lppns: Dict[str, str],
    corpus_weights: Dict[str, float],
    already_keys: Set[str],
    max_total_b: int = 850,
) -> List[Tuple[str, int, int]]:
    """Build Pass B: ply-3 continuations of Pass A + top frequent corpus plies 3..16."""
    tasks: Dict[str, Tuple[str, int, int]] = {}

    def add_pos(lp: str, mt: int, mpv: int):
        k = lppn5(lp)
        if k in already_keys:
            return
        prev = tasks.get(k)
        if prev is None or mt > prev[1]:
            tasks[k] = (lp, mt, mpv)

    # 1. Expand Pass A ply-2 verified book moves -> ply-3 (guaranteed 4-ply Tiền trận responses!)
    for rec in pass_a_results:
        if rec["ply"] != 2:
            continue
        pos_dec = from_lppn(rec["lppn"])
        legal = rules.legal_moves(pos_dec.board, pos_dec.turn)
        lmap = {lm.notation(pos_dec.board): lm for lm in legal}
        for ch in rec["chosen"][:2]:
            lm = lmap.get(ch["notation"])
            if lm is None:
                continue
            b3 = pos_dec.board.clone()
            rules.apply_move(b3, pos_dec.turn, lm)
            ck, clp, _ = canonical_lppn(b3, rules.other(pos_dec.turn), 3)
            add_pos(clp, 850, 3)

    # 2. Expand top Pass A ply-1 responses for all 10 ply-1 positions -> ply-2 -> ply-3
    # (already covered for top 8 ply-0, this adds the remaining 2 edge drops b1/b9!)
    for rec in pass_a_results:
        if rec["ply"] != 1:
            continue
        pos_dec = from_lppn(rec["lppn"])
        legal = rules.legal_moves(pos_dec.board, pos_dec.turn)
        lmap = {lm.notation(pos_dec.board): lm for lm in legal}
        for ch in rec["chosen"][:3]:
            lm = lmap.get(ch["notation"])
            if lm is None:
                continue
            b2 = pos_dec.board.clone()
            rules.apply_move(b2, pos_dec.turn, lm)
            ck, clp, _ = canonical_lppn(b2, rules.other(pos_dec.turn), 2)
            add_pos(clp, 850, 3)

    print(f"[Pass B] Seeded {len(tasks)} ply-2/3 Tiền trận continuations from Pass A", flush=True)

    # 3. Add highest-frequency corpus positions (plies 2..16) from Arena 4 & HvH
    ranked_corpus = sorted(corpus_weights.items(), key=lambda kv: (-kv[1], int(corpus_lppns[kv[0]].split()[-1])))
    added_corpus = 0
    for ck, w in ranked_corpus:
        if len(tasks) >= max_total_b:
            break
        if ck in already_keys or ck in tasks:
            continue
        lp = corpus_lppns[ck]
        ply = int(lp.split()[-1])
        if ply > MAX_BOOK_PLY:
            continue
        # Require at least 2 appearances or strong Arena 4 2000ms game for deep plies
        if ply >= 6 and w < 3.0:
            continue
        if ply >= 10 and w < 4.0:
            continue
        mt = 850 if ply <= 4 else (750 if ply <= 8 else 650)
        mpv = 3 if ply <= 8 else 2
        add_pos(lp, mt, mpv)
        added_corpus += 1

    print(f"[Pass B] Added {added_corpus} high-value corpus positions (plies 2..{MAX_BOOK_PLY}) -> total {len(tasks)}", flush=True)
    out = list(tasks.values())
    out.sort(key=lambda x: int(x[0].split()[-1]))
    return out


def export_all_outputs(all_results: List[dict], wdl_stats: Dict[Tuple[str, str], List[int]]) -> None:
    # 1. Deduplicate C++ (key, packed_move) -> (score, weight)
    cpp_dedup: Dict[Tuple[int, int], Tuple[int, int]] = {}
    # 2. GUI JSON: lppn5 -> {move_not: {move, weight, score, win, draw, loss, comment}}
    gui_book: Dict[str, Dict[str, dict]] = collections.defaultdict(dict)

    def _register(key: int, lp5: str, ply: int, packed: int, notation: str, score: int, weight: int, depth: int):
        w_eff = weight
        wdl = wdl_stats.get((lp5, notation), [0, 0, 0])
        tot = wdl[0] + wdl[1] + wdl[2]
        if tot >= 3:
            wr = (wdl[0] + 0.5 * wdl[1]) / tot
            # Gentle empirical adjustment: +/- 15% based on real Arena 4 / HvH winrate
            w_eff = max(10, int(round(w_eff * (0.85 + 0.30 * wr))))

        prev = cpp_dedup.get((key, packed))
        if prev is None or w_eff > prev[1]:
            cpp_dedup[(key, packed)] = (score, w_eff)

        prev_g = gui_book[lp5].get(notation)
        if prev_g is None or w_eff > prev_g["weight"]:
            sign = "+" if score > 0 else ""
            gui_book[lp5][notation] = {
                "move": notation,
                "weight": int(w_eff),
                "score": int(score),
                "win": int(wdl[0]),
                "draw": int(wdl[1]),
                "loss": int(wdl[2]),
                "comment": f"Homei v4-4 d{depth} ({sign}{score} cp) [ply {ply}]",
            }

    for rec in all_results:
        ply = rec["ply"]
        for ch in rec["chosen"]:
            _register(rec["key"], rec["lppn5"], ply, ch["packed"], ch["notation"], ch["score"], ch["weight"], ch["depth"])
        if rec["mkey"] != rec["key"]:
            for mch in rec["mir_chosen"]:
                _register(rec["mkey"], rec["mir_lppn5"], ply, mch["packed"], mch["notation"], mch["score"], mch["weight"], mch["depth"])

    sorted_rows = sorted(
        ((k, pm, sc, w) for (k, pm), (sc, w) in cpp_dedup.items()),
        key=lambda x: (x[0], -x[3], -x[2], x[1]),
    )
    unique_keys = len({k for k, _, _, _ in sorted_rows})

    # Write 1: HomeiEngine/src/book_data.h
    body = "\n".join(
        f"    {{{k}ULL, 0x{pm:08X}U, {sc}, {w}}}," for k, pm, sc, w in sorted_rows
    )
    header = f"""#pragma once

#include <cstdint>

namespace homei::book {{

struct BookEntry {{
    uint64_t key;
    uint32_t packed_move;
    int16_t score;
    uint16_t weight;
}};

inline constexpr BookEntry BOOK_ENTRIES[] = {{
{body}
}};
inline constexpr int BOOK_COUNT = {len(sorted_rows)};

}} // namespace homei::book
"""
    BOOK_DATA_H.write_text(header, encoding="utf-8")

    # Write 2: Homei/books/homei_v45_book.clpbook
    CLPBOOK_OUT.parent.mkdir(parents=True, exist_ok=True)
    clp_lines = [
        f"# Homei v4-5 Opening Book (Step 6b) | positions={unique_keys} entries={len(sorted_rows)} | Split Zobrist v4-4",
    ]
    clp_lines.extend(f"{k} 0x{pm:08X} {sc} {w}" for k, pm, sc, w in sorted_rows)
    CLPBOOK_OUT.write_text("\n".join(clp_lines) + "\n", encoding="utf-8")

    # Write 3: ColongphuongGUI_2/books/homei_v45_book.json
    GUI_JSON_OUT.parent.mkdir(parents=True, exist_ok=True)
    gui_export = {
        lp5: sorted(mvs.values(), key=lambda x: (-x["weight"], -x["score"], x["move"]))
        for lp5, mvs in sorted(gui_book.items())
    }
    GUI_JSON_OUT.write_text(json.dumps(gui_export, ensure_ascii=False, indent=2), encoding="utf-8")

    # Breakdown by ply
    ply_hist = collections.Counter(rec["ply"] for rec in all_results)
    print(f"\n[EXPORT] book_data.h   : {len(sorted_rows)} entries across {unique_keys} unique positions -> {BOOK_DATA_H}")
    print(f"[EXPORT] .clpbook      : {CLPBOOK_OUT} ({CLPBOOK_OUT.stat().st_size:,} bytes)")
    print(f"[EXPORT] GUI JSON book : {len(gui_export)} positions -> {GUI_JSON_OUT} ({GUI_JSON_OUT.stat().st_size:,} bytes)")
    print(f"[EXPORT] Canonical ply breakdown: {dict(sorted(ply_hist.items()))}", flush=True)


def main() -> None:
    t_all = time.perf_counter()
    assert ENGINE_EXE.is_file(), f"Engine not found: {ENGINE_EXE}"

    corpus_lppns, corpus_weights, wdl_stats = mine_corpus()

    pass_a_tasks = _build_pass_a_positions()
    pass_a_res = run_parallel_eval(pass_a_tasks, "Pass A (Plies 0..2)")
    already_keys = {r["lppn5"] for r in pass_a_res}

    pass_b_tasks = build_pass_b_positions(pass_a_res, corpus_lppns, corpus_weights, already_keys, max_total_b=850)
    pass_b_res = run_parallel_eval(pass_b_tasks, "Pass B (Plies 3..16)")

    all_res = pass_a_res + pass_b_res
    export_all_outputs(all_res, wdl_stats)
    print(f"\n[DONE] Step 6b book build complete in {time.perf_counter() - t_all:.1f}s!", flush=True)


if __name__ == "__main__":
    main()

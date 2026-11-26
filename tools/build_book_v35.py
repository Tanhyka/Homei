"""Deep-verified Opening Book generator (ply 0..8) for Homei_v3-5.

Combines:
1. Canonical non-self-blocking 4-ply reserve setups (L*bX + P*bY) & common XANH/CAM responses
2. All frequent strong human opening trees (ply 0..8) from clean HvH games
3. Refutation lines against poor AI openings (e.g. L*b5 + P*b5)
Verified via MultiPV=3 search with Homei_v3-5 (post-Texel eval!) and expanded with
horizontal mirror symmetry + Zobrist keys queried straight from the compiled engine!
Writes HomeiEngine/src/book_data.h.
"""
from __future__ import annotations

import collections
import concurrent.futures
import json
import re
import sys
import time
from pathlib import Path

HOMEI_ROOT = Path(r"E:\all-code\Code1\CLP_engine\Homei")
sys.path.insert(0, str(HOMEI_ROOT))
sys.path.insert(0, str(HOMEI_ROOT / "HomeiEngine" / "tests"))

from clp_ep_client import CLPEPClient, build_state_from_lppn, build_state_from_lppn_and_moves
from ref.clp import board as clp_board
from ref.clp import lppn as clp_lppn
from ref.clp import pieces as clp_pieces
from ref.clp import record_io
from ref.clp import rules as clp_rules
from tools.extract_koma_data import _mirror_board_and_turn, _mirror_move

ENGINE_EXE = HOMEI_ROOT / "HomeiEngine" / "build" / "homei.exe"
BOOK_DATA_H = HOMEI_ROOT / "HomeiEngine" / "src" / "book_data.h"
START_LPPN = "lppn1 X2K2S2T2V2T2S2K2X2/9/B2B2B2B2B2B2B2B2B2/9/9/9/b0b0b0b0b0b0b0b0b0/9/x0k0s0t0v0t0s0k0x0 c LP/LP -/- 0"

MOVE_KIND_MAP = {"single": 0, "team": 1, "rotate": 2, "drop": 3}
PTYPE_MAP = {"VUONG": 0, "TUONG": 1, "SI": 2, "KI": 3, "XA": 4, "LONG": 5, "PHUONG": 6, "BINH": 7}


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


def _mirror_notation(b: clp_board.Board, turn: clp_pieces.Player, mv_not: str) -> str:
    legal = clp_rules.legal_moves(b, turn)
    matched = [m for m in legal if m.canonical_notation(b) == mv_not]
    if len(matched) != 1:
        return ""
    mb = _mirror_board_and_turn(b, turn)
    mmv = _mirror_move(matched[0], mb)
    return mmv.canonical_notation(mb)


def _collect_opening_lppns() -> list[tuple[str, int]]:
    """Collect unique opening (lppn, human_freq) positions for plies 0..7."""
    pos_counts: collections.Counter[str] = collections.Counter()
    pos_lppn: dict[str, str] = {}

    def _board_key(lp: str) -> str:
        return " ".join(lp.split()[:5])

    def _add_seq(moves: list[str], weight: int) -> None:
        b = clp_board.Board()
        b.setup_initial()
        turn = clp_pieces.Player.CAM
        for ply, mstr in enumerate(moves):
            lp = clp_lppn.to_lppn(b, turn, ply=ply, strict=True)
            bk = _board_key(lp)
            pos_counts[bk] += weight
            pos_lppn.setdefault(bk, lp)
            if ply >= 7:
                break
            legal = clp_rules.legal_moves(b, turn)
            matched = [m for m in legal if m.canonical_notation(b) == mstr]
            if len(matched) != 1:
                break
            clp_rules.apply_move(b, turn, matched[0])
            turn = clp_rules.other(turn)

    # 1. Startpos (high weight) + strong human games parsed cleanly from texel_manifest.tsv (or HvH fallback)
    _add_seq([], 50)
    manifest_p = Path(r"C:\Users\Turtleplayshogi\.gemini\antigravity\brain\47e54bb1-e276-456a-ba00-5af60a33017f\scratch\texel_manifest.tsv")
    if manifest_p.is_file():
        for raw in manifest_p.read_text(encoding="utf-8", errors="ignore").splitlines():
            parts = raw.split("\t")
            if len(parts) == 4 and parts[2] == START_LPPN:
                mvs = parts[3].split()[:8]
                if len(mvs) >= 4:
                    _add_seq(mvs, 2)
    else:
        hvh_dir = Path(r"E:\all-code\Code1\CLP_dataset\LPGF\HvH")
        for fp in sorted(hvh_dir.glob("*.lpgf")):
            txt = fp.read_text(encoding="utf-8", errors="ignore")
            if "HISTORICAL_ILLEGAL_LOSS" in txt:
                continue
            rec = record_io.ImportedRecord()
            if not record_io.RecordIO.read(str(fp), rec):
                continue
            from ref.clp import core_bridge
            br = core_bridge.CoreBridge()
            app, _, _ = record_io.RecordIO.load_txt_moves_into_bridge(br, rec)
            if app < 6:
                continue
            nodes = br._active_line_nodes()
            mvs = [nd.move["notation"] for nd in nodes[1:9] if nd.move and "notation" in nd.move]
            if len(mvs) >= 4:
                _add_seq(mvs, 2)

    # 2. Seed key canonical ply-1 CAM drops + critical depth-7 counterplay lines (e.g. L*b5 -> P*h3/P*h7/L*h4/L*h6)
    for m1 in [f"L*b{c}" for c in range(1, 6)] + [f"P*b{c}" for c in range(1, 6)]:
        _add_seq([m1], 8)
        for m2 in ["P*h3", "P*h7", "L*h4", "L*h6", "L*h8", "L*h2"]:
            _add_seq([m1, m2], 3)
            for m3 in ["P*b5", "P*b7", "P*b3", "L*b4", "L*b6", "L*b8", "L*b2"]:
                _add_seq([m1, m2, m3], 2)

    # Keep important early opening nodes up to ply 6
    out = []
    for bk, cnt in pos_counts.most_common():
        lp = pos_lppn[bk]
        ply = int(lp.split()[-1])
        if ply <= 1 or (ply <= 3 and cnt >= 2) or cnt >= 4:
            out.append((lp, cnt))
        if len(out) >= 160:
            break
    return out


def _eval_chunk(chunk: list[tuple[str, int]]) -> list[tuple[int, int, int, int]]:
    """Run deep MultiPV search on each opening position & its horizontal mirror."""
    entries: list[tuple[int, int, int, int]] = []  # (zobrist_u64, packed_move, score, weight)
    with CLPEPClient(ENGINE_EXE) as client:
        client.set_option("OwnBook", 0)
        for lp, freq in chunk:
            ply = int(lp.split()[-1])
            if ply <= 1:
                mt, req_d, mpv = 4500, 7, 5
            elif ply <= 3:
                mt, req_d, mpv = 2400, 6, 3
            else:
                mt, req_d, mpv = 1800, 6, 3

            st, b, turn = build_state_from_lppn(lp)
            client.set_position_lppn(lp)
            client.send_line("hash")
            hline = client.read_line()
            key = int(hline.split()[1])

            out = client.search_movetime(st, movetime_ms=mt, depth=req_d, nodes=50000000, multipv=mpv)
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
            tol = 18 if ply <= 1 else 10
            chosen: list[tuple[dict, int, int]] = []
            for idx, inf in enumerate(top_infos):
                sc_info = inf["score"]
                if sc_info["kind"] != "cp":
                    continue
                sc = int(sc_info["value"])
                mv = inf["pv"][0]
                if mv["notation"] in ("L*h5", "P*h5") and ply == 1:
                    # Prefer active flank/offset counterplay (P*h3, P*h7, L*h4, L*h6, L*h8)
                    continue
                if ply == 0 and mv["notation"] == "P*b5":
                    # Avoid passive P*b5 opening when strong L*b6 / L*b4 / P*b7 / P*b3 are tied/equal
                    continue
                if sc < best_sc - tol:
                    continue
                diff = max(0, best_sc - sc)
                w = max(12, int(100 - diff * 4))
                if idx == 0:
                    w = int(w * 1.25)
                chosen.append((mv, sc, w))
            if not chosen:
                sc0 = int(top_infos[0]["score"]["value"])
                chosen.append((top_infos[0]["pv"][0], sc0, 100))

            for mv, sc, w in chosen:
                entries.append((key, pack_move_dict(mv), sc, w))

            # Add exact horizontal mirror entry using mirrored board + move (exact mirror key!)
            mb = _mirror_board_and_turn(b, turn)
            mlp = clp_lppn.to_lppn(mb, turn, ply=ply, strict=True)
            client.set_position_lppn(mlp)
            client.send_line("hash")
            mkey = int(client.read_line().split()[1])
            if mkey != key:
                mlegal = clp_rules.legal_moves(mb, turn)
                orig_legal = clp_rules.legal_moves(b, turn)
                for mv, sc, w in chosen:
                    matched = [lm for lm in orig_legal if lm.canonical_notation(b) == mv["notation"]]
                    if len(matched) == 1:
                        mmv = _mirror_move(matched[0], mb)
                        mn = mmv.canonical_notation(mb)
                        m_matched = [lm for lm in mlegal if lm.canonical_notation(mb) == mn]
                        if len(m_matched) == 1:
                            # Build dict from matched move
                            md = {
                                "kind": m_matched[0].kind,
                                "src": list(m_matched[0].src) if m_matched[0].src is not None else None,
                                "dst": list(m_matched[0].dst) if m_matched[0].dst is not None else None,
                                "ptype": m_matched[0].ptype.name,
                                "facing": m_matched[0].facing,
                                "rotate_after": m_matched[0].rotate_after,
                            }
                            entries.append((mkey, pack_move_dict(md), sc, w))
    return entries


def main() -> None:
    t0 = time.perf_counter()
    nodes = _collect_opening_lppns()
    print(f"[build_book] Deep-searching {len(nodes)} opening positions with {ENGINE_EXE.name}...")

    workers = 4
    chunks = [nodes[i::workers] for i in range(workers)]
    all_entries: list[tuple[int, int, int, int]] = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as pool:
        for res in pool.map(_eval_chunk, chunks):
            all_entries.extend(res)

    # Deduplicate (key, packed_move) & sort strictly by (key, -weight) for C++ lower_bound lookup
    dedup: dict[tuple[int, int], tuple[int, int]] = {}
    for k, pm, sc, w in all_entries:
        prev = dedup.get((k, pm))
        if prev is None or w > prev[1]:
            dedup[(k, pm)] = (sc, w)

    sorted_rows = sorted(
        ((k, pm, sc, w) for (k, pm), (sc, w) in dedup.items()),
        key=lambda x: (x[0], -x[3], x[1]),
    )

    body = "\n".join(
        f"    {{{k}ULL, 0x{pm:08X}U, {sc}, {w}}}," for k, pm, sc, w in sorted_rows
    )
    if not body:
        body = "    {0ULL, 0U, 0, 0}"

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
    n_keys = len({k for k, _, _, _ in sorted_rows})
    print(f"[build_book] Wrote {len(sorted_rows)} entries across {n_keys} unique positions to {BOOK_DATA_H} in {time.perf_counter() - t0:.1f}s")


if __name__ == "__main__":
    main()

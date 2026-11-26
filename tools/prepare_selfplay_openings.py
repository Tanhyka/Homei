"""Generate clean opening files for the 3-round selfplay tournament & Champion Self-Play:
  1. selfplay/book_openings.txt   - diverse 4..10 ply lines walked from Step 6b homei_v45_book.json
  2. selfplay/seeding_hvh.txt     - clean 8..20 ply real human middlegame positions from HvH_v2_draft
"""

from __future__ import annotations

import json
import random
import re
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple

GUI_ROOT = Path(r"E:\all-code\Code1\ColongphuongGUI_2")
SELFPLAY_ROOT = Path(r"E:\all-code\Code1\CLP_dataset\LPGF\selfplay")
HVH_DIR = Path(r"E:\all-code\Code1\CLP_dataset\LPGF\HvH_v2_draft")
BOOK_JSON = GUI_ROOT / "books" / "homei_v45_book.json"

sys.path.insert(0, str(GUI_ROOT))

from engine.board import Board
from engine.game import Game
from engine.lppn import to_lppn
from engine.notation import LPGFNotation
from engine.pieces import Player
from engine import rules


def lppn5(lp: str) -> str:
    parts = lp.strip().split()
    return " ".join(parts[:5]) if len(parts) >= 5 else lp.strip()


def build_book_openings(out_path: Path, target: int = 200) -> int:
    book: Dict[str, List[dict]] = json.loads(BOOK_JSON.read_text(encoding="utf-8"))
    rng = random.Random(20261126)

    seen_pos: Set[str] = set()
    lines: List[str] = []
    legal_cache: Dict[str, Dict[str, rules.Move]] = {}

    def get_legal_map(b: Board, turn: Player, k: str) -> Dict[str, rules.Move]:
        m = legal_cache.get(k)
        if m is None:
            m = {lm.notation(b): lm for lm in rules.legal_moves(b, turn)}
            legal_cache[k] = m
        return m

    # Fast BFS/DFS expansion of the book tree (plies 4..8) with zero redundant legal_moves calls!
    b0 = Board()
    b0.setup_initial()
    frontier: List[Tuple[Board, Player, List[str]]] = [(b0, Player.CAM, [])]
    visited_states: Set[str] = set()

    while frontier and len(lines) < target:
        idx = rng.randrange(len(frontier))
        b, turn, moves = frontier.pop(idx)
        ply = len(moves)
        lp = to_lppn(b, turn, ply=ply)
        k = lppn5(lp)
        if k in visited_states:
            continue
        visited_states.add(k)

        if 4 <= ply <= 8 and k not in seen_pos:
            seen_pos.add(k)
            lines.append(f"{lp}\t{' '.join(moves)}")
            if len(lines) >= target:
                break

        if ply >= 8:
            continue
        cands = book.get(k)
        if not cands:
            continue
        best_sc = max(int(c.get("score", 0)) for c in cands)
        good = [c for c in cands if int(c.get("score", 0)) >= best_sc - 12] or cands[:1]
        lmap = get_legal_map(b, turn, k)
        for ch in good[:4]:
            mv = lmap.get(ch["move"])
            if mv is None:
                continue
            b2 = b.clone()
            rules.apply_move(b2, turn, mv)
            frontier.append((b2, rules.other(turn), moves + [ch["move"]]))

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[BOOK OPENINGS] Wrote {len(lines)} unique in-book positions -> {out_path}", flush=True)
    return len(lines)


def _truncate_lpgf(txt: str, max_move_no: int = 24) -> str:
    out = []
    pat = re.compile(rf"^\s*{max_move_no + 1}\.")
    for line in txt.splitlines():
        if pat.match(line):
            break
        out.append(line)
    return "\n".join(out)


def build_seeding_hvh(out_path: Path, target: int = 240) -> int:
    rng = random.Random(20261127)
    files = sorted(HVH_DIR.glob("*.lpgf"))
    rng.shuffle(files)

    seen_pos: Set[str] = set()
    lines: List[str] = []

    for fp in files:
        if len(lines) >= target:
            break
        try:
            raw = fp.read_text(encoding="utf-8-sig", errors="ignore")
            if "HISTORICAL_ILLEGAL_LOSS" in raw:
                continue
            short_txt = _truncate_lpgf(raw, 24)
            g = Game()
            LPGFNotation.load_record(short_txt, g, fp.name, truncate_on_illegal=True)
        except Exception:
            continue

        nodes = []
        nd = g.root
        while nd is not None:
            nodes.append(nd)
            nd = nd.children[0] if nd.children else None
        if len(nodes) < 12:
            continue
        root_lp = to_lppn(nodes[0]._snapshot_board, nodes[0]._snapshot_current, ply=0)
        if "LP/LP" not in root_lp:
            continue

        # Pick 1..2 clean real-human middlegame checkpoints per game (ply 8..20, not in check, ongoing)
        cand_plies = [p for p in range(8, min(21, len(nodes) - 2))]
        rng.shuffle(cand_plies)
        picked = 0
        for ply in cand_plies:
            b = nodes[ply]._snapshot_board
            turn = nodes[ply]._snapshot_current
            if b is None or turn is None:
                continue
            if rules.is_in_check(b, turn):
                continue
            if rules.game_status(b, turn) != "ongoing":
                continue
            lp = to_lppn(b, turn, ply=ply)
            k = lppn5(lp)
            if k in seen_pos:
                continue
            # Reconstruct exact move list from ply 0..ply so match.py tracks full repetition history cleanly!
            mvs = []
            ok = True
            for i in range(1, ply + 1):
                pb = nodes[i - 1]._snapshot_board
                mv = nodes[i].move
                if pb is None or mv is None:
                    ok = False
                    break
                mvs.append(mv.notation(pb))
            if not ok:
                continue
            seen_pos.add(k)
            lines.append(f"{lp}\t{' '.join(mvs)}")
            picked += 1
            if picked >= 2 or len(lines) >= target:
                break

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[SEEDING HVH] Wrote {len(lines)} unique clean human positions -> {out_path}", flush=True)
    return len(lines)


def main() -> None:
    n_book = build_book_openings(SELFPLAY_ROOT / "book_openings.txt", target=200)
    n_seed = build_seeding_hvh(SELFPLAY_ROOT / "seeding_hvh.txt", target=240)
    assert n_book >= 50 and n_seed >= 50
    print(f"[DONE] Opening sets ready: Book={n_book}, Seeding={n_seed}", flush=True)


if __name__ == "__main__":
    main()

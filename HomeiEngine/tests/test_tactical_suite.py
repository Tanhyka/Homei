"""Test T/M — Tactical & Mate Suite T01-T27 (Section 6.3 of homei.md) + Step 4 Eval Symmetry."""

from __future__ import annotations

from typing import Tuple
import pytest

from clp_ep_client import (
    CLPEPClient,
    SearchOutput,
    build_state_from_lppn,
)
from positions import TACTICAL_POSITIONS, TacticalPosition, all_section6_lppns

from ref.clp import board as clp_board
from ref.clp import pieces as clp_pieces
from ref.clp import rules as clp_rules


def verify_legal_and_pv(
    out: SearchOutput, b: clp_board.Board, turn: clp_pieces.Player
) -> Tuple[str, str, int]:
    """Verifies bestmove legality, canonical notation, and PV legality.
    Returns (best_notation, score_str, pv_len)."""
    assert out.bestmove is not None, f"Engine returned no bestmove: {out.bestmove_raw}"
    bm = out.bestmove
    legal = clp_rules.legal_moves(b, turn)
    matched = [
        m for m in legal
        if m.kind == bm["kind"]
        and (list(m.src) if m.src is not None else None) == bm["src"]
        and (list(m.dst) if m.dst is not None else None) == bm["dst"]
        and (m.ptype.name if m.ptype is not None else "") == bm["ptype"]
        and m.facing == bm["facing"]
        and m.rotate_after == bm["rotate_after"]
    ]
    assert len(matched) == 1, f"bestmove {bm} is not in rules.legal_moves!"
    assert bm["notation"] == matched[0].canonical_notation(b), (
        f"bestmove notation {bm['notation']!r} != canonical {matched[0].canonical_notation(b)!r}"
    )

    assert out.infos, "Engine emitted no info lines"
    last_info = [info for info in out.infos if info.get("multipv", 1) == 1][-1]
    pv = last_info["pv"]
    assert len(pv) >= 1
    assert pv[0]["notation"] == bm["notation"], (
        f"bestmove {bm['notation']} != pv[0] {pv[0]['notation']}"
    )

    # Replay PV using rules.py
    sim_b = b.clone()
    sim_turn = turn
    for step_idx, pm in enumerate(pv):
        sim_legal = clp_rules.legal_moves(sim_b, sim_turn)
        p_match = [
            m for m in sim_legal
            if m.kind == pm["kind"]
            and (list(m.src) if m.src is not None else None) == pm["src"]
            and (list(m.dst) if m.dst is not None else None) == pm["dst"]
            and (m.ptype.name if m.ptype is not None else "") == pm["ptype"]
            and m.facing == pm["facing"]
            and m.rotate_after == pm["rotate_after"]
        ]
        assert len(p_match) == 1, f"Illegal PV move at step {step_idx}: {pm}"
        assert pm["notation"] == p_match[0].canonical_notation(sim_b)
        clp_rules.apply_move(sim_b, sim_turn, p_match[0])
        sim_turn = clp_rules.other(sim_turn)

    sc = last_info["score"]
    score_str = f"{sc['kind']} {sc['value']}"
    return bm["notation"], score_str, len(pv)


def check_mate_pv_terminal(out: SearchOutput, b: clp_board.Board, turn: clp_pieces.Player, max_ply: int) -> bool:
    if not out.infos or out.bestmove is None:
        return False
    last_info = [info for info in out.infos if info.get("multipv", 1) == 1][-1]
    sc = last_info["score"]
    if sc.get("kind") != "mate" or int(sc.get("value", 0)) <= 0:
        return False
    pv = last_info["pv"]
    if not (1 <= len(pv) <= max_ply):
        return False

    sim_b = b.clone()
    sim_turn = turn
    for pm in pv:
        sim_legal = clp_rules.legal_moves(sim_b, sim_turn)
        p_match = [m for m in sim_legal if m.canonical_notation(sim_b) == pm["notation"]]
        if len(p_match) != 1:
            return False
        clp_rules.apply_move(sim_b, sim_turn, p_match[0])
        sim_turn = clp_rules.other(sim_turn)

    return len(clp_rules.legal_moves(sim_b, sim_turn)) == 0


def test_step4_eval_symmetry():
    """Step 4 mandatory symmetry check (Section 4.3 item 7):
    eval(P) == eval(mirror_horizontal(P)) and eval_cam(P) == -eval_cam(flip_colors_180(P))."""
    with CLPEPClient() as client:
        for lppn_str in all_section6_lppns():
            client.set_position_lppn(lppn_str)
            client.send_line("eval")
            line = client.read_line(timeout=5.0)
            parts = line.split(" ")
            assert parts[0] == "eval" and len(parts) == 5
            ev, ev_mir, ev_cam, ev_flp_cam = map(int, parts[1:])
            assert ev == ev_mir, f"Mirror symmetry failed on {lppn_str}: {ev} != {ev_mir}"
            assert ev_cam == -ev_flp_cam, f"Flip-180 symmetry failed on {lppn_str}: {ev_cam} != -{ev_flp_cam}"


@pytest.mark.parametrize("pos", TACTICAL_POSITIONS, ids=[p.tid for p in TACTICAL_POSITIONS])
def test_tactical_position(pos: TacticalPosition):
    state, b, turn = build_state_from_lppn(pos.lppn)
    budget_ms = 5000 if pos.kind == "EXPECT" else 15000

    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=budget_ms, depth=32, nodes=50000000, multipv=1)
        not_str, score_str, pv_len = verify_legal_and_pv(out, b, turn)
        print(f"\n[RESULT] {pos.tid}: move={not_str}, score={score_str}, pv_len={pv_len}, time={out.elapsed_ms:.0f}ms")

        if pos.kind == "EXPECT":
            if not_str not in pos.expected:
                # Re-run at 30s per Section 6.0 rule
                out_30 = client.search_movetime(state, movetime_ms=30000, depth=32, nodes=50000000, multipv=1)
                not_30, score_30, _ = verify_legal_and_pv(out_30, b, turn)
                if not_30 not in pos.expected:
                    pytest.fail(
                        f"[{pos.tid}] 5s: move={not_str} ({score_str}, {out.elapsed_ms:.0f}ms) | "
                        f"30s: move={not_30} ({score_30}, {out_30.elapsed_ms:.0f}ms) | expected={pos.expected}"
                    )
        elif pos.kind == "MATE":
            ok = check_mate_pv_terminal(out, b, turn, pos.max_mate_ply)
            if not ok:
                out_30 = client.search_movetime(state, movetime_ms=30000, depth=32, nodes=50000000, multipv=1)
                not_30, score_30, pv_len_30 = verify_legal_and_pv(out_30, b, turn)
                ok_30 = check_mate_pv_terminal(out_30, b, turn, pos.max_mate_ply)
                assert ok_30, (
                    f"[{pos.tid}] Failed MATE({pos.max_mate_ply} ply) at 15s ({not_str}, {score_str}, pv_len={pv_len}) "
                    f"and 30s ({not_30}, {score_30}, pv_len={pv_len_30})"
                )
        elif pos.kind == "MATE_UNVERIFIED":
            ok = check_mate_pv_terminal(out, b, turn, pos.max_mate_ply)
            if not ok:
                pytest.skip(
                    f"UNVERIFIED [{pos.tid}]: No mate <= {pos.max_mate_ply} ply found "
                    f"(bestmove={not_str}, score={score_str}, time={out.elapsed_ms:.0f}ms)"
                )

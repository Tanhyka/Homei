"""Test Mate Correctness Suite (Step 1 of Phase 6: Homei v2-2).

Validates zero false positive mates on S1-S8 and T01-T28, verifies all sub-branches
of T28, and ensures repetition history (Thien Nhat Thu) prevents false mates.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import List, Tuple
import pytest

TESTS_DIR = Path(__file__).resolve().parent
HOMEI_ROOT = TESTS_DIR.parent.parent
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

from clp_ep_client import (
    CLPEPClient,
    SearchOutput,
    build_state_from_lppn,
    build_state_from_lppn_and_moves,
)
from positions import SYNTHETIC_POSITIONS, TACTICAL_POSITIONS, TacticalPosition
from test_tactical_suite import check_mate_pv_terminal, verify_legal_and_pv

from ref.clp import board as clp_board
from ref.clp import pieces as clp_pieces
from ref.clp import rules as clp_rules

T28_LPPN = (
    "lppn1 1B2B26/2S2S2V21T21P2/2B2B2B2B21B21/6b02/3B21P23/9/L24b0b0b0b0/"
    "3t0x11B11x0/8v0 x -/- TKKXBBB/TSSKKXLB 119"
)

MATE_TIDS = {"T23", "T24", "T25", "T27"}
NON_MATE_TIDS = {f"T{i:02d}" for i in range(1, 22)}


def _check_terminal(out: SearchOutput, b: clp_board.Board, turn: clp_pieces.Player) -> bool:
    if not out.infos or out.bestmove is None:
        return False
    last_info = [i for i in out.infos if i.get("multipv", 1) == 1][-1]
    pv = last_info.get("pv", [])
    if not pv:
        return False
    sim_b = b.clone()
    sim_turn = turn
    for pm in pv:
        legal = clp_rules.legal_moves(sim_b, sim_turn)
        matched = [m for m in legal if m.canonical_notation(sim_b) == pm["notation"]]
        if len(matched) != 1:
            return False
        clp_rules.apply_move(sim_b, sim_turn, matched[0])
        sim_turn = clp_rules.other(sim_turn)
    return len(clp_rules.legal_moves(sim_b, sim_turn)) == 0


def test_mate_zero_false_positives_and_real_mates():
    """Verify zero false positives on non-mate positions and valid mate PVs on mate positions."""
    with CLPEPClient() as client:
        # 1. Test S1 and S2 (Mate-in-1 positions)
        for s_pos in SYNTHETIC_POSITIONS[:2]:
            state, b, turn = build_state_from_lppn(s_pos.lppn)
            out = client.search_movetime(state, movetime_ms=3000, depth=32, nodes=50000000, multipv=1)
            not_str, score_str, pv_len = verify_legal_and_pv(out, b, turn)
            assert score_str == "mate 1", f"{s_pos.sid} expected 'mate 1', got {score_str}"
            assert _check_terminal(out, b, turn), f"{s_pos.sid} PV is not terminal!"

        # 2. Test S3-S8 (Non-mate synthetic positions: must NOT report positive mate)
        for s_pos in SYNTHETIC_POSITIONS[2:]:
            state, b, turn = build_state_from_lppn(s_pos.lppn)
            out = client.search_movetime(state, movetime_ms=3000, depth=32, nodes=50000000, multipv=1)
            last_info = [i for i in out.infos if i.get("multipv", 1) == 1][-1]
            sc = last_info["score"]
            if sc.get("kind") == "mate":
                assert int(sc.get("value", 0)) <= 0, f"{s_pos.sid} false positive mate: {sc}"

        # 3. Test T01-T21 (Non-mate tactical positions: must NOT report positive mate)
        for pos in TACTICAL_POSITIONS:
            if pos.tid in NON_MATE_TIDS:
                state, b, turn = build_state_from_lppn(pos.lppn)
                out = client.search_movetime(state, movetime_ms=2500, depth=32, nodes=50000000, multipv=1)
                last_info = [i for i in out.infos if i.get("multipv", 1) == 1][-1]
                sc = last_info["score"]
                if sc.get("kind") == "mate":
                    assert int(sc.get("value", 0)) <= 0, f"{pos.tid} false positive mate: {sc}"

        # 4. Test T23, T24, T25, T27 (Tactical mate positions)
        for pos in TACTICAL_POSITIONS:
            if pos.tid in MATE_TIDS:
                state, b, turn = build_state_from_lppn(pos.lppn)
                out = client.search_movetime(state, movetime_ms=5000, depth=32, nodes=50000000, multipv=1)
                not_str, score_str, pv_len = verify_legal_and_pv(out, b, turn)
                last_info = [i for i in out.infos if i.get("multipv", 1) == 1][-1]
                sc = last_info["score"]
                assert sc.get("kind") == "mate" and int(sc.get("value", 0)) > 0, (
                    f"{pos.tid} expected positive mate score, got {score_str}"
                )
                assert _check_terminal(out, b, turn), f"{pos.tid} mate PV not terminal!"


def test_t28_root_and_all_subbranches():
    """Verify T28 root solution and all sub-branches described in test_progress.md."""
    with CLPEPClient() as client:
        # A. Root T28 (ply 119)
        state, b, turn = build_state_from_lppn(T28_LPPN)
        out = client.search_movetime(state, movetime_ms=5000, depth=32, nodes=50000000, multipv=1)
        not_str, score_str, pv_len = verify_legal_and_pv(out, b, turn)
        last_info = [i for i in out.infos if i.get("multipv", 1) == 1][-1]
        sc = last_info["score"]
        assert not_str == "K*a3<", f"T28 root expected K*a3<, got {not_str}"
        assert sc.get("kind") == "mate" and 13 <= int(sc.get("value", 0)) <= 15
        assert _check_terminal(out, b, turn)

        # B. Ta3 defense branch: 1. K*a3< 2. Ta3< 3. K*c3< -> 4. Va6 / Va5 / Va4 / Vb4
        branch_ta3 = ["K*a3<", "Ta3<", "K*c3<"]
        for king_mv in ["Va6", "Va5", "Va4", "Vb4"]:
            st, b_k, t_k = build_state_from_lppn_and_moves(T28_LPPN, branch_ta3 + [king_mv])
            out_k = client.search_movetime(st, movetime_ms=3000, depth=32, nodes=50000000, multipv=1)
            not_k, score_k, _ = verify_legal_and_pv(out_k, b_k, t_k)
            sc_k = [i for i in out_k.infos if i.get("multipv", 1) == 1][-1]["score"]
            assert sc_k.get("kind") == "mate" and int(sc_k.get("value", 0)) > 0, (
                f"Branch Ta3< -> Kc3< -> {king_mv} expected mate score, got {score_k}"
            )
            assert _check_terminal(out_k, b_k, t_k), f"Branch {king_mv} PV not terminal!"

        # C. After 9. Sb1v (ply 128, CAM to move): 10. Vb2 / Vb1 / Vd2 / Vd1
        prefix_sb1v = [
            "K*a3<", "Va4", "S*b5>", "Va3", "X*a4>", "Vb2", "S*a1v", "Vc1", "Sb1v"
        ]
        for king_mv in ["Vb2", "Vb1", "Vd2", "Vd1"]:
            st, b_k, t_k = build_state_from_lppn_and_moves(T28_LPPN, prefix_sb1v + [king_mv])
            out_k = client.search_movetime(st, movetime_ms=3000, depth=32, nodes=50000000, multipv=1)
            not_k, score_k, _ = verify_legal_and_pv(out_k, b_k, t_k)
            sc_k = [i for i in out_k.infos if i.get("multipv", 1) == 1][-1]["score"]
            assert sc_k.get("kind") == "mate" and int(sc_k.get("value", 0)) > 0, (
                f"After Sb1v -> {king_mv} expected mate score, got {score_k}"
            )
            assert _check_terminal(out_k, b_k, t_k), f"After {king_mv} PV not terminal!"


def test_repetition_thien_nhat_thu_no_false_mate():
    """Verify that repetition count=2 prevents solver from reporting mate by 3rd repetition."""
    # S8 base with 7 moves leading to count=2 repetition for Vi5
    s8_base = "lppn1 4V24/9/8B2/9/9/9/9/t07l0/4v04 c -/- -/- 20"
    s8_moves = ["Va6", "Vi6", "Va5", "Vi5", "Va6", "Vi6", "Va5"]
    state, b, turn = build_state_from_lppn_and_moves(s8_base, s8_moves)

    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=2000, depth=6, nodes=50000000, multipv=1)
        not_str, score_str, _ = verify_legal_and_pv(out, b, turn)
        assert not_str != "Vi5", "Engine played 3rd repetition move Vi5!"
        last_info = [i for i in out.infos if i.get("multipv", 1) == 1][-1]
        sc = last_info["score"]
        if sc.get("kind") == "mate":
            assert int(sc.get("value", 0)) <= 0

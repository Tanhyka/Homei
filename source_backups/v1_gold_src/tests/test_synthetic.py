"""Test S1-S8 — Synthetic Verification Suite (Section 6.4 of homei.md)."""

from __future__ import annotations

import copy
import pytest

from clp_ep_client import (
    CLPEPClient,
    build_state_from_lppn,
    build_state_from_lppn_and_moves,
)
from positions import SYNTHETIC_POSITIONS
from test_tactical_suite import check_mate_pv_terminal, verify_legal_and_pv

from ref.clp import lppn as clp_lppn
from ref.clp import rules as clp_rules


def test_s1_mate1_split_or_rotate():
    """S1: Mate in 1 by team split / pawn rotation. Score must be mate 1."""
    s1 = SYNTHETIC_POSITIONS[0]
    state, b, turn = build_state_from_lppn(s1.lppn)
    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=5000, depth=32, nodes=50000000, multipv=1)
        not_str, score_str, pv_len = verify_legal_and_pv(out, b, turn)
        print(f"\n[RESULT] S1: move={not_str}, score={score_str}, time={out.elapsed_ms:.0f}ms")
        assert not_str in s1.expected, f"S1 move {not_str} not in {s1.expected}"
        assert score_str == "mate 1", f"S1 expected score 'mate 1', got {score_str!r}"
        assert check_mate_pv_terminal(out, b, turn, 1)


def test_s2_mate1_prisoner_drop():
    """S2: Mate in 1 by prisoner drop (X*a7<, X*a8<, X*b9^)."""
    s2 = SYNTHETIC_POSITIONS[1]
    state, b, turn = build_state_from_lppn(s2.lppn)
    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=5000, depth=32, nodes=50000000, multipv=1)
        not_str, score_str, pv_len = verify_legal_and_pv(out, b, turn)
        print(f"\n[RESULT] S2: move={not_str}, score={score_str}, time={out.elapsed_ms:.0f}ms")
        assert not_str in s2.expected, f"S2 move {not_str} not in {s2.expected}"
        assert score_str == "mate 1", f"S2 expected score 'mate 1', got {score_str!r}"
        assert check_mate_pv_terminal(out, b, turn, 1)


def test_s3_evade_check_and_avoid_mate_in_1():
    """S3: Evade check by Long (40 legal moves) and ensure CAM has no mate-in-1 after the chosen move."""
    s3 = SYNTHETIC_POSITIONS[2]
    state, b, turn = build_state_from_lppn(s3.lppn)
    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=5000, depth=32, nodes=50000000, multipv=1)
        not_str, score_str, _ = verify_legal_and_pv(out, b, turn)
        print(f"\n[RESULT] S3: move={not_str}, score={score_str}, time={out.elapsed_ms:.0f}ms")
        assert not_str in s3.expected, f"S3 move {not_str} not in {s3.expected}"

        # Exhaustively verify with rules.py that CAM does not have mate in 1 after `not_str`
        legal = clp_rules.legal_moves(b, turn)
        mv = [m for m in legal if m.canonical_notation(b) == not_str][0]
        b2 = b.clone()
        clp_rules.apply_move(b2, turn, mv)
        cam_turn = clp_rules.other(turn)
        for cam_mv in clp_rules.legal_moves(b2, cam_turn):
            b3 = b2.clone()
            clp_rules.apply_move(b3, cam_turn, cam_mv)
            assert len(clp_rules.legal_moves(b3, turn)) > 0, (
                f"After S3 move {not_str}, CAM has mate-in-1 with {cam_mv.canonical_notation(b2)}!"
            )


def test_s4_capture_undefended_long():
    """S4: Phuong captures undefended Long (Pe5)."""
    s4 = SYNTHETIC_POSITIONS[3]
    state, b, turn = build_state_from_lppn(s4.lppn)
    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=5000, depth=32, nodes=50000000, multipv=1)
        not_str, score_str, _ = verify_legal_and_pv(out, b, turn)
        print(f"\n[RESULT] S4: move={not_str}, score={score_str}, time={out.elapsed_ms:.0f}ms")
        assert not_str in s4.expected


def test_s5_avoid_capturing_defended_pawn_with_long():
    """S5: Binh at e5 is defended by XANH Long at e9; CAM must not play Le5."""
    s5 = SYNTHETIC_POSITIONS[4]
    state, b, turn = build_state_from_lppn(s5.lppn)
    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=5000, depth=32, nodes=50000000, multipv=1)
        not_str, score_str, _ = verify_legal_and_pv(out, b, turn)
        print(f"\n[RESULT] S5: move={not_str}, score={score_str}, time={out.elapsed_ms:.0f}ms")
        assert not_str not in s5.forbidden
        assert not_str in s5.expected


def test_s6_long_captures_attacking_pawn():
    """S6: Long at e5 is attacked by Binh at f5; CAM plays Lf5."""
    s6 = SYNTHETIC_POSITIONS[5]
    state, b, turn = build_state_from_lppn(s6.lppn)
    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=5000, depth=32, nodes=50000000, multipv=1)
        not_str, score_str, _ = verify_legal_and_pv(out, b, turn)
        print(f"\n[RESULT] S6: move={not_str}, score={score_str}, time={out.elapsed_ms:.0f}ms")
        assert not_str in s6.expected


def test_s7_save_entire_team_from_xa():
    """S7: Team [B2T2K0] at e5 is attacked by Xa at h5; CAM moves the whole team [K] away from col 5."""
    s7 = SYNTHETIC_POSITIONS[6]
    state, b, turn = build_state_from_lppn(s7.lppn)
    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=5000, depth=32, nodes=50000000, multipv=1)
        not_str, score_str, _ = verify_legal_and_pv(out, b, turn)
        print(f"\n[RESULT] S7: move={not_str}, score={score_str}, time={out.elapsed_ms:.0f}ms")
        assert not_str in s7.expected


def test_s8_avoid_threefold_repetition_loss():
    """S8: Replay 7 moves so cycle states have count=2; XANH must not play Vi5 (which causes count=3 loss),
    and verify reverse control when repetition history is stripped."""
    s8 = SYNTHETIC_POSITIONS[7]
    state, b, turn = build_state_from_lppn_and_moves(s8.lppn, s8.history_moves)
    assert clp_lppn.to_lppn(b, turn, ply=state["ply"]) == s8.current_lppn
    legal = clp_rules.legal_moves(b, turn)
    assert len(legal) >= 30

    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=5000, depth=6, nodes=50000000, multipv=1)
        not_str, score_str, _ = verify_legal_and_pv(out, b, turn)
        print(f"\n[RESULT] S8: move={not_str}, score={score_str}, time={out.elapsed_ms:.0f}ms")
        assert not_str != "Vi5", "Engine played Vi5 into 3-fold repetition loss!"
        for info in out.infos:
            if info.get("multipv", 1) == 1 and info.get("pv"):
                assert info["pv"][0]["notation"] != "Vi5"

        # Reverse control: strip repetition.entries to count=1 (fresh state from current_lppn)
        fresh_state, b_fresh, turn_fresh = build_state_from_lppn(s8.current_lppn)
        out_fresh = client.search_movetime(fresh_state, movetime_ms=1000, depth=4, nodes=50000000, multipv=1)
        verify_legal_and_pv(out_fresh, b_fresh, turn_fresh)

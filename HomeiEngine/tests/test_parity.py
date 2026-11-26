"""Test 0 — Rules & State Parity Suite (Section 6.1 of homei.md)."""

from __future__ import annotations

import copy
import random
from typing import List, Tuple
import pytest

from clp_ep_client import (
    CLPEPClient,
    build_state_from_lppn,
    build_state_from_lppn_and_moves,
)
from positions import all_section6_lppns

from ref.clp import board as clp_board
from ref.clp import core_bridge
from ref.clp import lppn as clp_lppn
from ref.clp import pieces as clp_pieces
from ref.clp import rules as clp_rules


def _generate_200_diverse_positions(seed: int = 20260405) -> List[Tuple[clp_board.Board, clp_pieces.Player, int, str]]:
    """Generates 200 valid diverse positions using rules.py (0-150 plies),
    guaranteeing coverage of stack >= 3, in-check, and all prisoner types."""
    rng = random.Random(seed)
    positions: List[Tuple[clp_board.Board, clp_pieces.Player, int, str]] = []
    seen_lppn = set()

    def add_pos(b: clp_board.Board, turn: clp_pieces.Player, ply: int) -> bool:
        s = clp_lppn.to_lppn(b, turn, ply=ply)
        if s in seen_lppn:
            return False
        seen_lppn.add(s)
        positions.append((b.clone(), turn, ply, s))
        return True

    # 1. Initial position and reserve drops
    b0 = clp_board.Board()
    b0.setup_initial()
    add_pos(b0, clp_pieces.Player.CAM, 0)

    # 2. Random playouts up to 150 plies
    while len(positions) < 190:
        b = clp_board.Board()
        b.setup_initial()
        turn = clp_pieces.Player.CAM
        max_plies = rng.randint(1, 150)
        for ply in range(1, max_plies + 1):
            legal = clp_rules.legal_moves(b, turn)
            if not legal:
                break
            # Bias slightly toward team, stack-building, and capture moves to get rich states
            weights = []
            for mv in legal:
                w = 1.0
                if mv.kind == "team":
                    w = 3.0
                elif mv.kind == "single" and mv.dst is not None and not b.sq(*mv.dst).is_empty:
                    w = 4.0
                weights.append(w)
            mv = rng.choices(legal, weights=weights, k=1)[0]
            clp_rules.apply_move(b, turn, mv)
            turn = clp_rules.other(turn)

            has_stack3 = any(len(b.sq(r, c).stack) >= 3 for r in range(9) for c in range(9))
            in_chk = clp_rules.is_in_check(b, turn)
            if has_stack3 or in_chk or ply in (1, 2, 3, 4) or rng.random() < 0.15:
                if len(positions) < 190:
                    add_pos(b, turn, ply)

    # 3. Ensure explicit coverage of stack >= 3, in-check, rotate_conflict, and all prisoner types
    for lppn_str in all_section6_lppns():
        if len(positions) >= 200:
            break
        dec = clp_lppn.from_lppn(lppn_str, strict=True)
        add_pos(dec.board, dec.turn, dec.ply)

    while len(positions) < 200:
        b = clp_board.Board()
        b.setup_initial()
        turn = clp_pieces.Player.CAM
        for ply in range(1, 40):
            legal = clp_rules.legal_moves(b, turn)
            if not legal:
                break
            mv = rng.choice(legal)
            clp_rules.apply_move(b, turn, mv)
            turn = clp_rules.other(turn)
            if len(positions) < 200:
                add_pos(b, turn, ply)

    return positions[:200]


DIVERSE_200 = _generate_200_diverse_positions()


def test_0_1_initial_position_and_perft():
    """Test 0.1: Opening position has 18 legal moves for CAM, 18 for XANH, perft(1)=18, perft(2)=324,
    and after reserve phase matches rules.legal_moves."""
    with CLPEPClient() as client:
        b = clp_board.Board()
        b.setup_initial()
        turn = clp_pieces.Player.CAM
        lppn0 = clp_lppn.to_lppn(b, turn, ply=0)

        resp = client.set_position_lppn(lppn0)
        assert resp == "positionok ply 0"

        cam_legal = client.get_legal_notations()
        py_cam_legal = sorted(m.canonical_notation(b) for m in clp_rules.legal_moves(b, turn))
        assert len(cam_legal) == 18
        assert cam_legal == py_cam_legal

        assert client.get_perft(1) == 18
        assert client.get_perft(2) == 324

        # Apply 1st move for CAM -> XANH has 18 legal moves
        first_move = clp_rules.legal_moves(b, turn)[0]
        resp1 = client.apply_notation(first_move.canonical_notation(b))
        assert resp1 == "positionok ply 1"
        clp_rules.apply_move(b, turn, first_move)
        turn = clp_rules.other(turn)

        xanh_legal = client.get_legal_notations()
        py_xanh_legal = sorted(m.canonical_notation(b) for m in clp_rules.legal_moves(b, turn))
        assert len(xanh_legal) == 18
        assert xanh_legal == py_xanh_legal

        # Complete all 4 reserve drops (ply 2, 3, 4) and verify legal moves match after reserve is empty
        for expected_ply in (2, 3, 4):
            mv = clp_rules.legal_moves(b, turn)[0]
            r = client.apply_notation(mv.canonical_notation(b))
            assert r == f"positionok ply {expected_ply}"
            clp_rules.apply_move(b, turn, mv)
            turn = clp_rules.other(turn)

        post_reserve_legal = client.get_legal_notations()
        py_post_reserve = sorted(m.canonical_notation(b) for m in clp_rules.legal_moves(b, turn))
        assert len(post_reserve_legal) == len(py_post_reserve)
        assert post_reserve_legal == py_post_reserve


def test_0_2_random_200_positions_legal_parity():
    """Test 0.2: 200 diverse valid positions (including stacks >= 3, in-check, all prisoner types)
    match rules.legal_moves 100% in count, notation, and all 6 structural fields."""
    assert len(DIVERSE_200) == 200

    # Verify dataset diversity requirements from Section 6.1.2
    has_stack3 = any(
        len(b.sq(r, c).stack) >= 3
        for b, _, _, _ in DIVERSE_200
        for r in range(9)
        for c in range(9)
    )
    has_check = any(clp_rules.is_in_check(b, turn) for b, turn, _, _ in DIVERSE_200)
    prisoner_types = {
        pt
        for b, _, _, _ in DIVERSE_200
        for side in (clp_pieces.Player.CAM, clp_pieces.Player.XANH)
        for pt in b.prisoners[side]
    }
    assert has_stack3, "Dataset must contain stacks of size >= 3"
    assert has_check, "Dataset must contain in-check positions"
    assert prisoner_types == {
        clp_pieces.PieceType.TUONG,
        clp_pieces.PieceType.SI,
        clp_pieces.PieceType.KI,
        clp_pieces.PieceType.XA,
        clp_pieces.PieceType.LONG,
        clp_pieces.PieceType.PHUONG,
        clp_pieces.PieceType.BINH,
    }

    with CLPEPClient() as client:
        for idx, (b, turn, ply, lppn_str) in enumerate(DIVERSE_200):
            client.set_position_lppn(lppn_str)
            eng_notations = client.get_legal_notations()
            eng_dicts = client.get_legal_dicts()

            py_moves = clp_rules.legal_moves(b, turn)
            py_notations = sorted(m.canonical_notation(b) for m in py_moves)

            assert len(eng_notations) == len(py_moves), f"Count mismatch at pos #{idx}: {lppn_str}"
            assert eng_notations == py_notations, f"Notation list mismatch at pos #{idx}: {lppn_str}"

            eng_tuples = {
                (
                    d["kind"],
                    tuple(d["src"]) if d["src"] is not None else None,
                    tuple(d["dst"]) if d["dst"] is not None else None,
                    d["ptype"],
                    d["facing"],
                    d["rotate_after"],
                    d["notation"],
                )
                for d in eng_dicts
            }
            py_tuples = {
                (
                    m.kind,
                    m.src,
                    m.dst,
                    m.ptype.name if m.ptype is not None else "",
                    m.facing,
                    m.rotate_after,
                    m.canonical_notation(b),
                )
                for m in py_moves
            }
            assert eng_tuples == py_tuples, f"6-field structural mismatch at pos #{idx}: {lppn_str}"


def test_0_3_apply_parity_first_50_positions():
    """Test 0.3: For every legal move of the first 50 positions, applying the move in the engine
    produces the exact same LPPN as rules.apply_move + lppn.to_lppn."""
    with CLPEPClient() as client:
        for idx, (b, turn, ply, lppn_str) in enumerate(DIVERSE_200[:50]):
            py_moves = clp_rules.legal_moves(b, turn)
            for mv in py_moves:
                not_str = mv.canonical_notation(b)
                client.set_position_lppn(lppn_str)
                resp = client.apply_notation(not_str)
                assert resp == f"positionok ply {ply + 1}", f"Apply failed at pos #{idx} move {not_str}: {resp}"
                eng_lppn = client.get_lppn()

                b_copy = b.clone()
                clp_rules.apply_move(b_copy, turn, mv)
                py_lppn = clp_lppn.to_lppn(b_copy, clp_rules.other(turn), ply=ply + 1)
                assert eng_lppn == py_lppn, (
                    f"LPPN mismatch after apply at pos #{idx} move {not_str}:\n"
                    f"  engine: {eng_lppn}\n  python: {py_lppn}"
                )


def test_0_4_adjudicate_parity():
    """Test 0.4: Adjudicate parity across 4 scenarios:
    (1) repetition3, (2) checkmate, (3) PAT, and (4) simultaneous repetition3 + no legal moves (repetition3 wins first)."""
    with CLPEPClient() as client:
        # Scenario 1: 3-fold repetition via S8 cycle + Vi5
        s8_base = "lppn1 4V24/9/8B2/9/9/9/9/t07l0/4v04 c -/- -/- 20"
        s8_moves = ["Va6", "Vi6", "Va5", "Vi5", "Va6", "Vi6", "Va5", "Vi5"]
        py_state_rep, _, _ = build_state_from_lppn_and_moves(s8_base, s8_moves)
        assert py_state_rep["result"] == {
            "status": "terminal",
            "reason": "repetition3",
            "winner": "CAM",
            "loser": "XANH",
        }
        client.set_position_lppn(s8_base)
        for m in s8_moves:
            assert client.apply_notation(m).startswith("positionok ")
        eng_state_rep = client.get_state_dict()
        assert eng_state_rep["result"] == py_state_rep["result"]

        # Scenario 2: Checkmate via S1 + Tb9^
        s1_base = "lppn1 v08/2B36/[L2T0]8/9/9/9/9/9/4V24 c -/- -/- 10"
        py_state_mate, _, _ = build_state_from_lppn_and_moves(s1_base, ["Tb9^"])
        assert py_state_mate["result"] == {
            "status": "terminal",
            "reason": "checkmate",
            "winner": "CAM",
            "loser": "XANH",
        }
        client.set_position_lppn(s1_base)
        assert client.apply_notation("Tb9^") == "positionok ply 11"
        eng_state_mate = client.get_state_dict()
        assert eng_state_mate["result"] == py_state_mate["result"]

        # Scenario 3: Stalemate (PAT) via S1 + Bb8^
        py_state_pat, _, _ = build_state_from_lppn_and_moves(s1_base, ["Bb8^"])
        assert py_state_pat["result"] == {
            "status": "terminal",
            "reason": "pat",
            "winner": "CAM",
            "loser": "XANH",
        }
        client.set_position_lppn(s1_base)
        assert client.apply_notation("Bb8^") == "positionok ply 11"
        eng_state_pat = client.get_state_dict()
        assert eng_state_pat["result"] == py_state_pat["result"]

        # Scenario 4: Simultaneous 3-fold repetition AND opponent has 0 legal moves (checkmate) -> repetition3 takes priority!
        sim_state, _, _ = build_state_from_lppn_and_moves(s1_base, ["Tb9^"])
        # Force current position's repetition count to 3 while status is sent as "ongoing" so engine adjudicates it
        sim_state["repetition"]["entries"][-1]["count"] = 3
        sim_state["result"] = {"status": "ongoing"}
        core_bridge._adjudicate_state_py(sim_state)
        assert sim_state["result"] == {
            "status": "terminal",
            "reason": "repetition3",
            "winner": "XANH",
            "loser": "CAM",
        }
        # Send with ongoing status to engine and verify engine adjudicates repetition3 before checkmate
        raw_send = copy.deepcopy(sim_state)
        raw_send["result"] = {"status": "ongoing"}
        client.set_position_state(raw_send)
        eng_sim_state = client.get_state_dict()
        assert eng_sim_state["result"] == sim_state["result"]


def test_0_5_lppn_roundtrip_and_invalid_rejection():
    """Test 0.5: Every LPPN in Section 6 round-trips identically, and malformed LPPNs are rejected with error without crashing."""
    with CLPEPClient() as client:
        for lppn_str in all_section6_lppns():
            dec = clp_lppn.from_lppn(lppn_str, strict=True)
            py_rt = clp_lppn.to_lppn(dec.board, dec.turn, ply=dec.ply)
            assert py_rt == lppn_str

            resp = client.set_position_lppn(lppn_str)
            assert resp == f"positionok ply {dec.ply}"
            eng_rt = client.get_lppn()
            assert eng_rt == lppn_str

        invalid_lppns = [
            # Missing square in row 0 (only 8 squares)
            "lppn1 3V24/9/9/9/9/9/9/9/4v04 c -/- -/- 0",
            # 10 squares in row 0
            "lppn1 4V25/9/9/9/9/9/9/9/4v04 c -/- -/- 0",
            # 1-piece stack
            "lppn1 4V24/9/[B2]8/9/9/9/9/9/4v04 c -/- -/- 0",
            # Duplicate reserve
            "lppn1 4V24/9/9/9/9/9/9/9/4v04 c LL/- -/- 0",
            # King in hand
            "lppn1 4V24/9/9/9/9/9/9/9/4v04 c -/- V/- 0",
            # Mixed-owner stack
            "lppn1 4V24/9/[B2b0]8/9/9/9/9/9/4v04 c -/- -/- 0",
            # King in stack
            "lppn1 9/9/[B2V2]8/9/9/9/9/9/4v04 c -/- -/- 0",
            # Wrong number of fields
            "lppn1 4V24/9/9/9/9/9/9/9/4v04 c -/-",
        ]
        for bad in invalid_lppns:
            client.send_line(f"position lppn {bad}")
            line = client.read_line(timeout=5.0)
            assert line.startswith("error "), f"Expected error for malformed LPPN {bad!r}, got {line!r}"
            # Verify process did not crash
            client.send_line("isready")
            assert client.read_line(timeout=5.0) == "readyok"


def test_0_6_notation_parity_special_cases():
    """Test 0.6: Canonical notation parity including ambiguous .src, rotate_conflict, rotate-in-place, team, and drop moves."""
    special_lppns = [
        # S3: Two Tuongs on i6 and i4 both can move to h5 -> .i4 and .i6 disambiguation
        "lppn1 V23L24/9/9/9/9/9/5s03/3b01b03/3t0v0t03 x -/- -/B 10",
        # Rotate conflict: Binh at c5 moving onto friendly Binh at b5 (stacking) vs rotating at b5
        "lppn1 4V24/4B24/4B24/9/4[B2T2K0]4/9/9/9/4v04 c -/- LB/- 10",
        # T15 & T17: Rotate in place (.f5, .g9, .g7)
        "lppn1 X21S22T22X2/1L22T2V21S21/2B2B2B2[B2K3]B2B2B2/2P22K03/[B2B2]8/3b0b04/1[b0b0]b0p01b0b0b0b0/1l01s01t01s01/x0k02k0t01v0x0 x -/- -/- 37",
        "lppn1 X21S22T22X2/1L22T2V21S21/B21B2B2B2B2B2B2B2/B28/3P2K0K23/9/b0b0b0b0b0b0b0b0b0/1l01s0p0t01s01/x0k02[k0t0]2v0x0 x -/- -/- 27",
    ]
    with CLPEPClient() as client:
        for lppn_str in special_lppns:
            dec = clp_lppn.from_lppn(lppn_str, strict=True)
            client.set_position_lppn(lppn_str)
            eng_dicts = client.get_legal_dicts()
            py_moves = clp_rules.legal_moves(dec.board, dec.turn)

            eng_by_struct = {
                (
                    d["kind"],
                    tuple(d["src"]) if d["src"] is not None else None,
                    tuple(d["dst"]) if d["dst"] is not None else None,
                    d["ptype"],
                    d["facing"],
                    d["rotate_after"],
                ): d["notation"]
                for d in eng_dicts
            }
            for m in py_moves:
                key = (
                    m.kind,
                    m.src,
                    m.dst,
                    m.ptype.name if m.ptype is not None else "",
                    m.facing,
                    m.rotate_after,
                )
                assert key in eng_by_struct
                assert eng_by_struct[key] == m.canonical_notation(dec.board)


def test_0_7_pinned_ki_jump_over_king_and_pinner_parity():
    """Regression test for CLP-specific KI distance-2 jump along pin rays:
    a pinned KI must NOT jump over its own King or over the enemy pinner."""
    pinned_ki_lppns = [
        # Real game_0052 ply 85: Xanh KI at g6 pinned by Cam PHUONG at f5 to Xanh VUONG at h7; jumping over h7 to i8 is illegal
        "lppn1 X22T2V2T23/1S32B2S23/B1B21B2B21B1B21/6B21B2/1L22[p0b1]2x0X3/l01[b0b0]1P24/3k01b0b1b1B3/2v0s01s03/x0K33t03 x -/- TKBB/K 85",
        # Synthetic diagonal pin: Xanh KI at g6 pinned by Cam PHUONG at f5; jumping over f5 to e4 or over h7 to i8 is illegal
        "lppn1 4V24/9/9/9/9/4P24/3k05/2v06/9 x -/- -/- 20",
        # Synthetic orthogonal pin: Xanh KI at f5 pinned by Cam LONG at e5; jumping over e5 to d5 or over g5 to h5 is illegal
        "lppn1 4V24/9/9/9/4L24/4k04/4v04/9/9 x -/- -/- 20",
    ]
    with CLPEPClient() as client:
        for lp in pinned_ki_lppns:
            dec = clp_lppn.from_lppn(lp, strict=True)
            client.set_position_lppn(lp)
            eng_nots = {d["notation"] for d in client.get_legal_dicts()}
            py_nots = {m.canonical_notation(dec.board) for m in clp_rules.legal_moves(dec.board, dec.turn)}
            assert eng_nots == py_nots, f"Pinned-KI parity mismatch on {lp}: extra={eng_nots - py_nots}, missing={py_nots - eng_nots}"

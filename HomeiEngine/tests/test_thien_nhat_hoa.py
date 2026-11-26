"""Unit tests for Điều 5 (Thiên Nhật Hòa — unavoidable 3-fold repetition = Draw)
and Điều 4 (Thiên Nhật Thủ — avoidable 3-fold repetition = Loss).
"""

from __future__ import annotations

import copy
import sys
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
HOMEI_ROOT = TESTS_DIR.parent.parent
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

from clp_ep_client import CLPEPClient, build_state_from_lppn, decode_state64
from ref.clp import core_bridge, rules as clp_rules


def _make_forced_rep3_state(lppn: str, forced_move_not: str) -> dict:
    """Builds a valid clp.state/1 where playing `forced_move_not` produces count == 3."""
    state, b, turn = build_state_from_lppn(lppn)
    legal = clp_rules.legal_moves(b, turn)
    matched = [m for m in legal if m.canonical_notation(b) == forced_move_not]
    assert len(matched) == 1, f"Expected unique legal move {forced_move_not}, got {[m.canonical_notation(b) for m in legal]}"
    sim = b.clone()
    clp_rules.apply_move(sim, turn, matched[0])
    target_pos = core_bridge._encode_position_py(sim, clp_rules.other(turn))
    state["repetition"]["entries"].append({"count": 2, "position": copy.deepcopy(target_pos)})
    return state


def test_dieu5_unavoidable_rep3_adjudicate_and_search():
    """CAM King at a9 has ONLY ONE legal move (Va8), which leads to count == 3 -> Thiên Nhật Hòa (Draw)."""
    lppn = "lppn1 V28/8l0/9/9/9/9/9/9/8v0 c -/- -/- 10"
    state = _make_forced_rep3_state(lppn, "Va8")

    # 1. Python CoreBridge reference check
    bridge = core_bridge.CoreBridge()
    _, b, turn = build_state_from_lppn(lppn)
    mv = [m for m in clp_rules.legal_moves(b, turn) if m.canonical_notation(b) == "Va8"][0]
    next_py = bridge._apply_move_to_state(state, core_bridge._encode_move_py(mv, "Va8"))
    assert next_py["result"]["status"] == "terminal"
    assert next_py["result"]["reason"] == "repetition3_draw"
    assert next_py["result"]["winner"] is None
    assert next_py["result"]["loser"] is None

    # 2. C++ Engine adjudication + search score check
    with CLPEPClient() as client:
        client.set_position_state(state)
        out = client.search_movetime(state, movetime_ms=300, depth=6, nodes=100000, multipv=1)
        assert out.bestmove is not None
        assert out.bestmove["notation"] == "Va8"
        last_info = [i for i in out.infos if i.get("multipv", 1) == 1][-1]
        assert last_info["score"]["kind"] == "cp", f"Expected draw cp 0, got {last_info['score']}"
        assert last_info["score"]["value"] == 0, f"Expected draw cp 0, got {last_info['score']}"

        # Apply the forced move and verify engine adjudicates repetition3_draw
        client.set_position_state(state)
        client.send_line("apply Va8")
        posok = client.read_line(timeout=2.0)
        assert posok.startswith("positionok ")
        client.send_line("state64")
        st_line = client.read_line(timeout=2.0)
        assert st_line.startswith("state64 ")
        eng_state = decode_state64(st_line.split(" ", 1)[1])
        assert eng_state["result"]["status"] == "terminal"
        assert eng_state["result"]["reason"] == "repetition3_draw"
        assert eng_state["result"]["winner"] is None
        assert eng_state["result"]["loser"] is None


def test_dieu4_avoidable_rep3_still_loses_and_engine_avoids():
    """CAM has Va8 (causes rep3) AND a pawn move Bh9v (avoids rep3) -> Va8 is Điều 4 LOSS; engine plays Bh9v."""
    lppn = "lppn1 V28/8l0/9/9/9/9/B28/9/8v0 c -/- -/- 10"
    state = _make_forced_rep3_state(lppn, "Va8")

    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=300, depth=6, nodes=100000, multipv=1)
        assert out.bestmove is not None
        assert out.bestmove["notation"] != "Va8", "Engine must avoid avoidable rep3 loss (Điều 4)"

        # If CAM deliberately plays Va8 anyway, engine adjudicates Điều 4 loss (XANH wins)
        client.set_position_state(state)
        client.send_line("apply Va8")
        posok = client.read_line(timeout=2.0)
        assert posok.startswith("positionok ")
        client.send_line("state64")
        st_line = client.read_line(timeout=2.0)
        eng_state = decode_state64(st_line.split(" ", 1)[1])
        assert eng_state["result"]["status"] == "terminal"
        assert eng_state["result"]["reason"] == "repetition3"
        assert eng_state["result"]["winner"] == "XANH"
        assert eng_state["result"]["loser"] == "CAM"


def test_dieu5_in_check_sole_evasion_rep3_is_not_mate():
    """XANH moves Pd6->Pc7 checking CAM V2 at a9; CAM's ONLY legal evasion is Va8 which hits count==3.
    By Điều 5 (Thiên Nhật Hòa), Pc7 MUST NOT be claimed as a forced mate win by df-pn or alpha-beta!"""
    lppn = "lppn1 V28/8l0/9/3p05/9/9/9/9/8v0 x -/- -/- 10"
    state, b, turn = build_state_from_lppn(lppn)
    # Build position after XANH Pc7 and CAM Va8
    sim = b.clone()
    pc7 = [m for m in clp_rules.legal_moves(sim, turn) if m.canonical_notation(sim) == "Pc7"][0]
    clp_rules.apply_move(sim, turn, pc7)
    va8 = [m for m in clp_rules.legal_moves(sim, clp_rules.other(turn)) if m.canonical_notation(sim) == "Va8"][0]
    clp_rules.apply_move(sim, clp_rules.other(turn), va8)
    target_pos = core_bridge._encode_position_py(sim, turn)
    state["repetition"]["entries"].append({"count": 2, "position": copy.deepcopy(target_pos)})

    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=500, depth=6, nodes=150000, multipv=1)
        assert out.bestmove is not None
        last_info = [i for i in out.infos if i.get("multipv", 1) == 1][-1]
        if out.bestmove["notation"] == "Pc7":
            assert last_info["score"]["kind"] != "mate", f"Pc7 allows Thiên Nhật Hòa draw, must not report mate: {last_info['score']}"

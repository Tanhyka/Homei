"""Test P — CLP-EP v1 Protocol & Robustness Suite (Section 6.2 of homei.md)."""

from __future__ import annotations

import copy
import json
import time
from typing import List
import pytest

from clp_ep_client import (
    CLPEPClient,
    b64url_decode,
    b64url_encode,
    build_state_from_lppn,
    build_state_from_lppn_and_moves,
    default_engine_path,
)

from ref.clp import board as clp_board
from ref.clp import core_bridge
from ref.clp import engine_client as clp_engine_client
from ref.clp import lppn as clp_lppn
from ref.clp import pieces as clp_pieces
from ref.clp import rules as clp_rules


def _assert_move_is_legal(move_dict: dict, b: clp_board.Board, turn: clp_pieces.Player) -> None:
    assert isinstance(move_dict, dict)
    assert set(move_dict.keys()) == {
        "schema", "kind", "src", "dst", "ptype", "facing", "rotate_after", "notation"
    }
    assert move_dict["schema"] == "clp.move/1"
    legal = clp_rules.legal_moves(b, turn)
    matched = [
        m for m in legal
        if m.kind == move_dict["kind"]
        and (list(m.src) if m.src is not None else None) == move_dict["src"]
        and (list(m.dst) if m.dst is not None else None) == move_dict["dst"]
        and (m.ptype.name if m.ptype is not None else "") == move_dict["ptype"]
        and m.facing == move_dict["facing"]
        and m.rotate_after == move_dict["rotate_after"]
    ]
    assert len(matched) == 1, f"Returned move {move_dict} not in rules.legal_moves"
    assert move_dict["notation"] == matched[0].canonical_notation(b)


def test_p_1_handshake_and_engine_client_compatibility():
    """Test P.1: Full handshake lines per Section 3.3, readyok after isready,
    and verified with ref.clp.engine_client.EngineClient."""
    with CLPEPClient() as client:
        required_lines = {
            "id name Homei",
            "protocol clp-ep 1",
            "capability search legal",
            "capability clocks movetime",
            "capability search infinite",
            "capability control stop",
            "option name Depth type spin default 32 min 1 max 32",
            "option name Nodes type spin default 50000000 min 1 max 50000000",
            "option name MultiPV type spin default 1 min 1 max 5",
            "clpok",
        }
        assert required_lines.issubset(set(client.handshake_lines))
        assert client.handshake_lines[-1] == "clpok"

    # Also test directly with ref.clp.engine_client.EngineClient
    from PyQt6.QtCore import QCoreApplication
    app = QCoreApplication.instance() or QCoreApplication([])
    ec = clp_engine_client.EngineClient()
    messages: List[str] = []
    ready_events: List[bool] = []
    ec.message.connect(messages.append)
    ec.engineReady.connect(ready_events.append)

    ec.launch(str(default_engine_path()))
    deadline = time.perf_counter() + 5.0
    while time.perf_counter() < deadline and not ec.ready():
        app.processEvents()
        time.sleep(0.01)

    assert ec.ready() is True
    assert ec.engineName() == "Homei"
    assert ec.supportsMovetime() is True
    assert ec.supportsInfinite() is True
    assert ec.supportsStop() is True
    assert ec.supportsDepth() is True
    assert ec.supportsNodes() is True
    assert "Engine ready (capabilities verified)" in messages
    ec.shutdown()
    app.processEvents()


def test_p_2_sequence_position_setoptions_go():
    """Test P.2: Sequence position state64 -> positionok ply n -> 3x setoption -> 3x optionok -> go."""
    lppn_str = "lppn1 X2K2S2T2V2T21K2X2/9/B2B2B2B2B21B2B2B2/4L21[B2P2]2/3S35/9/b0[b0l0]b0b0b0b0b0b0b0/3s0p0s0t02/x0k01t01v01k0x0 x -/- -/- 15"
    state, b, turn = build_state_from_lppn(lppn_str)

    with CLPEPClient() as client:
        pos_resp = client.set_position_state(state)
        assert pos_resp == "positionok ply 15"

        assert client.set_option("Depth", 4) == "optionok name Depth value 4"
        assert client.set_option("Nodes", 100000) == "optionok name Nodes value 100000"
        assert client.set_option("MultiPV", 1) == "optionok name MultiPV value 1"

        client.send_line("go id 7 movetime 500")
        infos = []
        bestmove_obj = None
        while True:
            line = client.read_line(timeout=5.0)
            if line.startswith("info id 7 info64 "):
                infos.append(json.loads(b64url_decode(line.split(" ", 4)[4]).decode("utf-8")))
            elif line.startswith("bestmove id 7 move64 "):
                bestmove_obj = json.loads(b64url_decode(line.split(" ", 4)[4]).decode("utf-8"))
                break
        assert len(infos) >= 1
        assert bestmove_obj is not None
        _assert_move_is_legal(bestmove_obj, b, turn)


def test_p_3_movetime_1ms_and_1000ms():
    """Test P.3: movetime=1 returns a valid legal bestmove; movetime=1000 finishes within 1000 ± 150 ms."""
    lppn_str = "lppn1 X2K21T2V24/1S22T14/B2B2B2B2B24/4[S2P2]2L2p0/6k32/6b02/b0[b0l0]b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- B/KXBBB 45"
    state, b, turn = build_state_from_lppn(lppn_str)

    with CLPEPClient() as client:
        # 1 ms search
        out_1ms = client.search_movetime(state, movetime_ms=1, depth=32, nodes=50000000, multipv=1)
        assert out_1ms.bestmove is not None
        _assert_move_is_legal(out_1ms.bestmove, b, turn)

        # 1000 ms search (must finish within 1000 ± 150 ms)
        out_1000ms = client.search_movetime(state, movetime_ms=1000, depth=32, nodes=50000000, multipv=1)
        assert out_1000ms.bestmove is not None
        _assert_move_is_legal(out_1000ms.bestmove, b, turn)
        assert 850.0 <= out_1000ms.elapsed_ms <= 1150.0, f"Expected 1000±150ms, got {out_1000ms.elapsed_ms:.1f}ms"


def test_p_4_depth_and_nodes_limits():
    """Test P.4: Depth=3 stops at depth <= 3; Nodes=20000 stops at <= 1.05x nodes (<= 21000)."""
    lppn_str = "lppn1 X2K21T2V24/1S22T14/B2B2B2B2B24/4[S2P2]2L2p0/6k32/6b02/b0[b0l0]b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- B/KXBBB 45"
    state, b, turn = build_state_from_lppn(lppn_str)

    with CLPEPClient() as client:
        out_depth = client.search_movetime(state, movetime_ms=10000, depth=3, nodes=50000000, multipv=1)
        assert out_depth.infos
        assert max(info["depth"] for info in out_depth.infos) <= 3
        assert out_depth.infos[-1]["depth"] == 3
        assert out_depth.bestmove is not None
        _assert_move_is_legal(out_depth.bestmove, b, turn)

        out_nodes = client.search_movetime(state, movetime_ms=10000, depth=32, nodes=20000, multipv=1)
        assert out_nodes.infos
        max_reported_nodes = max(info["nodes"] for info in out_nodes.infos)
        assert max_reported_nodes <= int(20000 * 1.05), f"Nodes {max_reported_nodes} exceeded 1.05 * 20000"
        assert out_nodes.bestmove is not None
        _assert_move_is_legal(out_nodes.bestmove, b, turn)


def test_p_5_multipv_3():
    """Test P.5: MultiPV=3 emits info with multipv = 1, 2, 3 having pairwise distinct first moves
    and score(1) >= score(2) >= score(3)."""
    lppn_str = "lppn1 X2K2S2T2V2T21K2X2/9/B2B2B2B2B21B2B2B2/4L21[B2P2]2/3S35/9/b0[b0l0]b0b0b0b0b0b0b0/3s0p0s0t02/x0k01t01v01k0x0 x -/- -/- 15"
    state, b, turn = build_state_from_lppn(lppn_str)

    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=2000, depth=3, nodes=50000000, multipv=3)
        by_depth = {}
        for info in out.infos:
            by_depth.setdefault(info["depth"], {})[info["multipv"]] = info

        assert 3 in by_depth
        d3 = by_depth[3]
        assert set(d3.keys()) == {1, 2, 3}

        first_moves = [d3[r]["pv"][0]["notation"] for r in (1, 2, 3)]
        assert len(set(first_moves)) == 3, f"MultiPV first moves not distinct: {first_moves}"

        scores = [d3[r]["score"]["value"] for r in (1, 2, 3)]
        assert scores[0] >= scores[1] >= scores[2], f"MultiPV scores not monotonic: {scores}"
        assert out.bestmove is not None
        assert out.bestmove["notation"] == first_moves[0]


def test_p_6_infinite_and_stop():
    """Test P.6: go id N infinite emits searchstarted id N immediately, info depth increases,
    stray stop id 999 is ignored, stop id N returns bestmove in <= 200 ms, and subsequent go works."""
    lppn_str = "lppn1 X2K21T2V24/1S22T14/B2B2B2B2B24/4[S2P2]2L2p0/6k32/6b02/b0[b0l0]b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- B/KXBBB 45"
    state, b, turn = build_state_from_lppn(lppn_str)

    with CLPEPClient() as client:
        client.set_position_state(state)
        client.set_option("MultiPV", 1)

        t_start = time.perf_counter()
        client.send_line("go id 42 infinite")
        first_line = client.read_line(timeout=1.0)
        start_latency_ms = (time.perf_counter() - t_start) * 1000.0
        assert first_line == "searchstarted id 42"
        assert start_latency_ms <= 200.0

        # Read a couple of info lines and verify depth increases
        depths = []
        while len(depths) < 2:
            line = client.read_line(timeout=5.0)
            if line.startswith("info id 42 info64 "):
                info_obj = json.loads(b64url_decode(line.split(" ", 4)[4]).decode("utf-8"))
                depths.append(info_obj["depth"])
        assert depths == sorted(depths) and depths[-1] >= 2

        # Send stray stop id 999 (must be ignored)
        client.send_line("stop id 999")

        # Send real stop id 42 and measure latency to bestmove id 42
        t_stop = time.perf_counter()
        client.send_line("stop id 42")
        bestmove_obj = None
        while True:
            line = client.read_line(timeout=2.0)
            if line.startswith("bestmove id 42 move64 "):
                bestmove_obj = json.loads(b64url_decode(line.split(" ", 4)[4]).decode("utf-8"))
                break
        stop_latency_ms = (time.perf_counter() - t_stop) * 1000.0
        assert stop_latency_ms <= 200.0, f"stop took {stop_latency_ms:.1f}ms (> 200ms)"
        assert bestmove_obj is not None
        _assert_move_is_legal(bestmove_obj, b, turn)

        # Follow up with a new search (id 43)
        out2 = client.search_movetime(state, movetime_ms=200, depth=2, nodes=50000000, multipv=1)
        assert out2.bestmove is not None
        _assert_move_is_legal(out2.bestmove, b, turn)


def test_p_7_terminal_and_no_legal_moves():
    """Test P.7: State with result.status == 'terminal' or checkmate position returns bestmove id N none."""
    with CLPEPClient() as client:
        # 1. Checkmate position (S1 after Bb8^)
        s1_base = "lppn1 v08/2B36/[L2T0]8/9/9/9/9/9/4V24 c -/- -/- 10"
        mate_state, _, _ = build_state_from_lppn_and_moves(s1_base, ["Bb8^"])
        out_mate = client.search_movetime(mate_state, movetime_ms=500, depth=4, nodes=100000, multipv=1)
        assert out_mate.bestmove is None
        assert out_mate.bestmove_raw == f"bestmove id {out_mate.req_id} none"

        # 2. Normal position explicitly marked result.status = "terminal"
        init_b = clp_board.Board()
        init_b.setup_initial()
        init_lppn = clp_lppn.to_lppn(init_b, clp_pieces.Player.CAM, ply=0)
        term_state, _, _ = build_state_from_lppn(init_lppn)
        term_state["result"] = {
            "status": "terminal",
            "reason": "checkmate",
            "winner": "XANH",
            "loser": "CAM",
        }
        out_term = client.search_movetime(term_state, movetime_ms=500, depth=4, nodes=100000, multipv=1)
        assert out_term.bestmove is None
        assert out_term.bestmove_raw == f"bestmove id {out_term.req_id} none"


def test_p_8_repetition_entries_avoid_third_repetition():
    """Test P.8: When repetition.entries has count=2 for a reachable position, engine avoids 3rd repetition."""
    s8_base = "lppn1 4V24/9/8B2/9/9/9/9/t07l0/4v04 c -/- -/- 20"
    s8_moves = ["Va6", "Vi6", "Va5", "Vi5", "Va6", "Vi6", "Va5"]
    state, b, turn = build_state_from_lppn_and_moves(s8_base, s8_moves)

    with CLPEPClient() as client:
        out = client.search_movetime(state, movetime_ms=1000, depth=6, nodes=50000000, multipv=1)
        assert out.bestmove is not None
        _assert_move_is_legal(out.bestmove, b, turn)
        assert out.bestmove["notation"] != "Vi5"


def test_p_9_robustness_2mib_bad_json_and_clean_quit():
    """Test P.9: >2 MiB line does not crash; JSON missing required keys returns error; quit exits in <= 1s."""
    client = CLPEPClient()
    client.start(do_handshake=True)
    try:
        # 1. Line > 2 MiB (2.2 MiB)
        huge_line = "position state64 " + ("A" * (2 * 1024 * 1024 + 200000))
        client.send_line(huge_line)
        resp_huge = client.read_line(timeout=5.0)
        assert resp_huge.startswith("error ")

        # Verify engine still responsive
        client.send_line("isready")
        assert client.read_line(timeout=5.0) == "readyok"

        # 2. JSON missing required keys
        bad_json = json.dumps({"schema": "clp.state/1", "ply": 0}).encode("utf-8")
        client.send_line("position state64 " + b64url_encode(bad_json))
        resp_bad = client.read_line(timeout=5.0)
        assert resp_bad.startswith("error ")

        client.send_line("isready")
        assert client.read_line(timeout=5.0) == "readyok"

        # 3. Clean quit in <= 1s
        t0 = time.perf_counter()
        client.send_line("quit")
        assert client.proc is not None
        rc = client.proc.wait(timeout=1.0)
        elapsed = time.perf_counter() - t0
        assert rc == 0
        assert elapsed <= 1.0
    finally:
        client.close()

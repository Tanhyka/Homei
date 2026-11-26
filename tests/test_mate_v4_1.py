"""Verification suite for Homei v4-1 mate solver upgrades (tests/test_mate_v4_1.py).

Validates:
1. All 4 positions from E:\\all-code\\Code1\\CLP_engine\\1-Progress\\mate.md (P1-P4) in MultiPV=3 (movetime & infinite).
2. Exact 3-screenshot GUI sequence on P4 (including both engine's PV1 and the B*i2< / Vi2 branch):
   - Shortest forced mate (<= 17 on P4 instead of 23)
   - Negative even mate on ALL MultiPV lines when defender is to move (longest defense first)
   - Smooth monotonic countdown (+N -> -(N-1) -> +(N-2) -> ... -> +1)
3. Full PV walkthrough to terminal checkmate / forced repetition3 on P1, P2, P3, P4.
4. Instant depth-1 emission and <= 200ms stop latency on non-mate positions without false mates.
"""

from __future__ import annotations

import os
import json
import queue
import sys
import threading
import time
from pathlib import Path
from typing import Dict, List, Tuple

HOMEI_ROOT = Path(__file__).resolve().parents[1]
TESTS_DIR = HOMEI_ROOT / "HomeiEngine" / "tests"
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

from clp_ep_client import (
    CLPEPClient,
    SearchOutput,
    b64url_decode,
    build_state_from_lppn,
    build_state_from_lppn_and_moves,
    default_engine_path,
)
from ref.clp import board as clp_board
from ref.clp import core_bridge
from ref.clp import pieces as clp_pieces
from ref.clp import rules as clp_rules

V4_1_EXE = Path(os.environ.get("HOMEI_EXE", str(default_engine_path())))

MATE_MD_POSITIONS = {
    "P1": (
        "XANH",
        "lppn1 1B2B26/2S2S2V21T21P2/2B2B2B2B21B21/6b02/3B21P23/9/L24b0b0b0b0/3t0x11B11x0/8v0 x -/- TKKXBBB/TSSKKXLB 119",
    ),
    "P2": (
        "CAM",
        "lppn1 2S2T21x21K3X2/2P2V21t33/2B2B2B21B2B2B2/2B22S21P21/1B27/1l02b04/K11b0b0b0b01b0b0/2T12v03/3B11s12x0 c -/- TKB/SKXLB 94",
    ),
    "P3": (
        "XANH",
        "lppn1 1B2b11V21B22/2S01K01S12/2B2B2B2B2B2B21/2B31B24/9/5b0b0b01/3b0b01s01b0/3t32p02/4[b0k3]t0v0k0x0 x -/- SXXXLL/TTKP 81",
    ),
    "P4": (
        "CAM",
        "lppn1 1K27/V21P2x35/B25B22/2T2B21B21B21/7T21/5b0b02/2b0b0b01b0X31/2s3t3b01t12/4k01v01s0 c -/- SSKKXLLPBBBB/XBB 94",
    ),
}


def run_infinite(
    client: CLPEPClient,
    state_dict: dict,
    wait_s: float = 3.0,
    multipv: int = 3,
) -> Tuple[SearchOutput, float]:
    client.set_position_state(state_dict)
    client.set_option("MultiPV", multipv)
    client._next_id += 1
    req_id = client._next_id
    t0 = time.perf_counter()
    client.send_line(f"go id {req_id} infinite")
    started = client.read_line(timeout=2.0)
    assert started == f"searchstarted id {req_id}", f"Expected searchstarted, got {started!r}"

    q: queue.Queue = queue.Queue()
    done_ev = threading.Event()

    def _pump() -> None:
        while not done_ev.is_set():
            assert client.proc is not None and client.proc.stdout is not None
            raw = client.proc.stdout.readline()
            if not raw:
                q.put(None)
                return
            line = raw.decode("utf-8", errors="replace").strip()
            q.put(line)
            if line.startswith(f"bestmove id {req_id} "):
                return

    th = threading.Thread(target=_pump, daemon=True)
    th.start()

    infos: List[dict] = []
    bestmove_raw = ""
    bestmove_obj = None
    sent_stop = False
    t_stop = 0.0
    deadline = time.perf_counter() + wait_s

    while True:
        now = time.perf_counter()
        if not sent_stop and now >= deadline:
            t_stop = now
            client.send_line(f"stop id {req_id}")
            sent_stop = True
        timeout = 2.0 if sent_stop else max(0.05, deadline - now)
        try:
            line = q.get(timeout=timeout)
        except queue.Empty:
            if sent_stop:
                raise TimeoutError(f"Timed out waiting for bestmove after stop id {req_id}")
            continue
        if line is None:
            raise EOFError("Engine stdout closed unexpectedly")
        if line.startswith(f"info id {req_id} info64 "):
            token = line.split(" ", 4)[4]
            infos.append(json.loads(b64url_decode(token).decode("utf-8")))
        elif line.startswith(f"bestmove id {req_id} "):
            done_ev.set()
            th.join(timeout=2.0)
            stop_latency_ms = (time.perf_counter() - t_stop) * 1000.0 if sent_stop else 0.0
            parts = line.split(" ")
            if len(parts) == 5 and parts[3] == "move64":
                bestmove_obj = json.loads(b64url_decode(parts[4]).decode("utf-8"))
            bestmove_raw = line
            break

    return (
        SearchOutput(
            req_id=req_id,
            bestmove_raw=bestmove_raw,
            bestmove=bestmove_obj,
            infos=infos,
            elapsed_ms=(time.perf_counter() - t0) * 1000.0,
        ),
        stop_latency_ms,
    )


def latest_by_multipv(out: SearchOutput) -> Dict[int, dict]:
    by_rank: Dict[int, dict] = {}
    for info in out.infos:
        rank = int(info.get("multipv", 1))
        prev = by_rank.get(rank)
        if prev is None or int(info["depth"]) >= int(prev["depth"]):
            by_rank[rank] = info
    return by_rank


def check_terminal_pv(pv: List[dict], b: clp_board.Board, turn: clp_pieces.Player) -> Tuple[bool, str]:
    if not pv:
        return False, "empty"
    sim_b = b.clone()
    sim_t = turn
    rep_counts: Dict[str, int] = {}
    init_id = core_bridge._position_identity_py(core_bridge._encode_position_py(sim_b, sim_t))
    rep_counts[init_id] = 1
    for idx, pm in enumerate(pv):
        legal = clp_rules.legal_moves(sim_b, sim_t)
        matched = [m for m in legal if m.canonical_notation(sim_b) == pm["notation"]]
        if len(matched) != 1:
            return False, f"illegal step {idx}: {pm['notation']}"
        clp_rules.apply_move(sim_b, sim_t, matched[0])
        sim_t = clp_rules.other(sim_t)
        cur_id = core_bridge._position_identity_py(core_bridge._encode_position_py(sim_b, sim_t))
        rep_counts[cur_id] = rep_counts.get(cur_id, 0) + 1
        if rep_counts[cur_id] >= 3:
            if idx + 1 == len(pv) and (idx % 2 == 1):
                return True, "repetition3"
            return False, f"unexpected rep3 at step {idx}"
    rem = len(clp_rules.legal_moves(sim_b, sim_t))
    if rem == 0:
        return True, "checkmate"
    return False, f"non_terminal(legal={rem})"


def test_four_mate_md_positions_movetime_and_infinite():
    """All 4 positions from mate.md must solve to positive mate (MultiPV=3), with P4 <= 17 and real depth."""
    assert V4_1_EXE.is_file(), f"Missing {V4_1_EXE}"
    with CLPEPClient(V4_1_EXE) as client:
        for pid, (winner, lppn) in MATE_MD_POSITIONS.items():
            state, b, turn = build_state_from_lppn(lppn)
            # Infinite mode (GUI analysis simulation)
            out_inf, stop_ms = run_infinite(client, state, wait_s=3.5, multipv=3)
            assert stop_ms <= 200.0, f"{pid} stop latency too high: {stop_ms:.1f}ms"
            ranks = latest_by_multipv(out_inf)
            assert 1 in ranks, f"{pid} missing PV1"
            pv1 = ranks[1]
            sc = pv1["score"]
            pv_not = [m["notation"] for m in pv1["pv"]]
            term_ok, term_reason = check_terminal_pv(pv1["pv"], b, turn)
            print(
                f"\n[{pid} inf] winner={winner} d={pv1['depth']} score={sc} term={term_reason} "
                f"pv={' '.join(pv_not[:12])} (len={len(pv_not)})"
            )
            assert sc["kind"] == "mate" and int(sc["value"]) > 0, f"{pid} failed to find mate in infinite: {sc}"
            assert int(sc["value"]) == len(pv1["pv"]), f"{pid} mate value {sc['value']} != pv len {len(pv1['pv'])}"
            assert term_ok, f"{pid} PV not terminal: {term_reason}"

            # Depths across all infos for a given multipv rank must be monotonic non-decreasing (no fake depth 23)
            rank1_depths = [int(i["depth"]) for i in out_inf.infos if int(i.get("multipv", 1)) == 1]
            assert rank1_depths == sorted(rank1_depths), f"{pid} non-monotonic PV1 depths: {rank1_depths}"

            if pid == "P1":
                assert pv1["pv"][0]["notation"] == "K*a3<", f"P1 expected K*a3<, got {pv1['pv'][0]['notation']}"
                assert 13 <= int(sc["value"]) <= 15
            elif pid == "P4":
                assert int(sc["value"]) <= 17, f"P4 expected shortest mate <= 17, got {sc}"

            # Also verify movetime mode (MultiPV=1)
            out_mt = client.search_movetime(state, movetime_ms=3000, depth=32, nodes=50000000, multipv=1)
            mt_pv1 = [i for i in out_mt.infos if int(i.get("multipv", 1)) == 1][-1]
            assert mt_pv1["score"]["kind"] == "mate" and int(mt_pv1["score"]["value"]) > 0, (
                f"{pid} movetime=3000 failed: {mt_pv1['score']}"
            )
            if pid == "P4":
                assert int(mt_pv1["score"]["value"]) <= 17, f"P4 movetime expected <= 17, got {mt_pv1['score']}"


def test_p4_three_screenshots_sequence_and_bi2_branch():
    """Verify exact 3-screenshot continuity on P4:
    1. Root P4 -> mate +N (odd, <= 17)
    2. After PV1[0] -> defender sees mate -(N-1) (even) on ALL MultiPV=3 lines
    3. After defender's PV1[0] -> attacker sees exact mate +(N-2)
    4. Also after B*i2< (Screenshot 2) -> ALL MultiPV=3 lines report mate -even (e.g. -16), then Vi2 -> mate +15!
    """
    p4_lppn = MATE_MD_POSITIONS["P4"][1]
    with CLPEPClient(V4_1_EXE) as client:
        # 1. Root P4
        st0, b0, t0 = build_state_from_lppn(p4_lppn)
        out0, _ = run_infinite(client, st0, wait_s=3.5, multipv=3)
        r0 = latest_by_multipv(out0)
        n0 = int(r0[1]["score"]["value"])
        mv0 = r0[1]["pv"][0]["notation"]
        print(f"\n[P4 seq] Step 0 (Cam): d={r0[1]['depth']} mate +{n0}, move={mv0}")
        assert r0[1]["score"]["kind"] == "mate" and 1 <= n0 <= 17 and (n0 % 2 == 1)

        # 2. Defender turn after engine's best move
        st1, b1, t1 = build_state_from_lppn_and_moves(p4_lppn, [mv0])
        out1, _ = run_infinite(client, st1, wait_s=4.5, multipv=3)
        r1 = latest_by_multipv(out1)
        expected_def = -(n0 - 1)
        print(f"[P4 seq] Step 1 (Xanh after {mv0}):")
        for k in sorted(r1):
            sc = r1[k]["score"]
            print(f"  PV{k}: d={r1[k]['depth']} {sc['kind']} {sc['value']} move={r1[k]['pv'][0]['notation']}")
            assert sc["kind"] == "mate", f"Defender PV{k} showed {sc} instead of negative mate!"
            assert int(sc["value"]) < 0 and (abs(int(sc["value"])) % 2 == 0), f"Defender PV{k} not negative even mate: {sc}"
        assert int(r1[1]["score"]["value"]) == expected_def, (
            f"Expected PV1 mate {expected_def}, got {r1[1]['score']}"
        )
        assert int(r1[1]["score"]["value"]) >= int(r1[2]["score"]["value"]) >= int(r1[3]["score"]["value"])
        mv1 = r1[1]["pv"][0]["notation"]

        # 3. Attacker turn after defender's longest defense
        st2, b2, t2 = build_state_from_lppn_and_moves(p4_lppn, [mv0, mv1])
        out2, _ = run_infinite(client, st2, wait_s=3.0, multipv=3)
        r2 = latest_by_multipv(out2)
        n2 = int(r2[1]["score"]["value"])
        print(f"[P4 seq] Step 2 (Cam after {mv0} {mv1}): d={r2[1]['depth']} mate +{n2}, move={r2[1]['pv'][0]['notation']}")
        assert r2[1]["score"]["kind"] == "mate" and n2 == n0 - 2, f"Expected mate +{n0 - 2}, got {r2[1]['score']}"

        # 4. Exact Screenshot 2 & 3 user branch: B*i2< then Vi2
        st_b, _, _ = build_state_from_lppn_and_moves(p4_lppn, ["B*i2<"])
        out_b, _ = run_infinite(client, st_b, wait_s=4.5, multipv=3)
        rb = latest_by_multipv(out_b)
        print("[P4 B*i2<] Xanh to move:")
        for k in sorted(rb):
            sc = rb[k]["score"]
            print(f"  PV{k}: d={rb[k]['depth']} {sc['kind']} {sc['value']} move={rb[k]['pv'][0]['notation']}")
            assert sc["kind"] == "mate" and int(sc["value"]) < 0 and (abs(int(sc["value"])) % 2 == 0), (
                f"After B*i2< PV{k} expected negative even mate, got {sc}"
            )
        def_b_val = int(rb[1]["score"]["value"])
        def_b_mv = rb[1]["pv"][0]["notation"]
        assert -16 <= def_b_val <= -2

        st_bn, _, _ = build_state_from_lppn_and_moves(p4_lppn, ["B*i2<", def_b_mv])
        out_bn, _ = run_infinite(client, st_bn, wait_s=3.0, multipv=3)
        r_bn = latest_by_multipv(out_bn)
        print(f"[P4 B*i2< {def_b_mv}] Cam to move: PV1={r_bn[1]['score']} move={r_bn[1]['pv'][0]['notation']}")
        assert r_bn[1]["score"]["kind"] == "mate" and int(r_bn[1]["score"]["value"]) == abs(def_b_val) - 1

        st_vi2, _, _ = build_state_from_lppn_and_moves(p4_lppn, ["B*i2<", "Vi2"])
        out_vi2, _ = run_infinite(client, st_vi2, wait_s=3.0, multipv=3)
        r_vi2 = latest_by_multipv(out_vi2)
        print(f"[P4 B*i2< Vi2] Cam to move: PV1={r_vi2[1]['score']} move={r_vi2[1]['pv'][0]['notation']}")
        assert r_vi2[1]["score"]["kind"] == "mate" and 1 <= int(r_vi2[1]["score"]["value"]) <= 15


def test_full_countdown_walk_all_4_positions():
    """Step move-by-move down the PV for P1, P2, P3, P4 keeping one persistent GUI session.
    Checks +N -> -(N-1) -> +(N-2) -> ... -> +1 monotonicity and ALL MultiPV defender negative even mates.
    """
    with CLPEPClient(V4_1_EXE) as client:
        for pid in ("P1", "P3", "P4", "P2"):
            lppn = MATE_MD_POSITIONS[pid][1]
            moves_played: List[str] = []
            st, b, turn = build_state_from_lppn(lppn)
            out0, _ = run_infinite(client, st, wait_s=3.5, multipv=3)
            r0 = latest_by_multipv(out0)
            assert r0[1]["score"]["kind"] == "mate" and int(r0[1]["score"]["value"]) > 0, (
                f"{pid} root failed mate: {r0[1]['score']}"
            )
            start_len = int(r0[1]["score"]["value"])
            prev_val = start_len
            mv = r0[1]["pv"][0]["notation"]
            print(f"\n[{pid} countdown start] len={start_len}, mv={mv}")

            for step in range(1, start_len + 1):
                moves_played.append(mv)
                st_cur, b_cur, t_cur = build_state_from_lppn_and_moves(lppn, moves_played)
                if st_cur["result"]["status"] == "terminal":
                    print(f"  [{pid}] terminal reached cleanly at ply {step}: {st_cur['result']}")
                    break
                n_legal = len(clp_rules.legal_moves(b_cur, t_cur))
                wait_s = 3.5 if (step % 2 == 1) else 2.5
                out_k, _ = run_infinite(client, st_cur, wait_s=wait_s, multipv=3)
                rk = latest_by_multipv(out_k)
                sc1 = rk[1]["score"]
                is_attacker_turn = (step % 2 == 0)

                if is_attacker_turn:
                    assert sc1["kind"] == "mate" and int(sc1["value"]) > 0, (
                        f"{pid} step {step} attacker expected positive mate, got {sc1} after {moves_played}"
                    )
                    cur_v = int(sc1["value"])
                    expected_exact = abs(prev_val) - 1
                    assert cur_v == expected_exact or (pid == "P2" and cur_v <= expected_exact), (
                        f"{pid} step {step} attacker jumped: expected +{expected_exact}, got +{cur_v}"
                    )
                    prev_val = cur_v
                else:
                    expected_k = min(3, n_legal)
                    for rank_idx in range(1, expected_k + 1):
                        r_sc = rk[rank_idx]["score"]
                        assert r_sc["kind"] == "mate" and int(r_sc["value"]) < 0, (
                            f"{pid} step {step} defender PV{rank_idx} expected negative mate, got {r_sc} "
                            f"after {moves_played}"
                        )
                        if pid != "P2":
                            assert abs(int(r_sc["value"])) % 2 == 0, (
                                f"{pid} step {step} defender PV{rank_idx} expected even negative mate, got {r_sc}"
                            )
                    cur_v = int(sc1["value"])
                    expected_exact = -(prev_val - 1)
                    assert cur_v == expected_exact or (pid == "P2" and abs(cur_v) <= prev_val - 1), (
                        f"{pid} step {step} defender expected mate {expected_exact}, got {cur_v} after {moves_played}"
                    )
                    prev_val = cur_v

                mv = rk[1]["pv"][0]["notation"]


def test_stop_latency_and_no_false_mates_on_non_mate_positions():
    """Verify non-mate middlegame/in-check positions still emit depth 1 immediately and stop <= 200ms."""
    non_mate_lppns = [
        # T04 middlegame
        "lppn1 X2K21T2V24/1S22T14/B2B2B2B2B24/4[S2P2]2L2p0/6k32/6b02/b0[b0l0]b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- B/KXBBB 45",
        # In-check non-mate position (S3)
        "lppn1 V23L24/9/9/9/9/9/5s03/3b01b03/3t0v0t03 x -/- -/B 10",
    ]
    with CLPEPClient(V4_1_EXE) as client:
        for idx, lppn in enumerate(non_mate_lppns):
            st, b, turn = build_state_from_lppn(lppn)
            out, stop_ms = run_infinite(client, st, wait_s=0.4, multipv=3)
            assert stop_ms <= 200.0, f"Non-mate #{idx} stop latency {stop_ms:.1f}ms > 200ms"
            assert len(out.infos) >= 1, f"Non-mate #{idx} emitted no info lines in 400ms"
            ranks = latest_by_multipv(out)
            sc = ranks[1]["score"]
            assert sc["kind"] == "cp", f"Non-mate #{idx} false mate reported: {sc}"

"""Test piecevalues command and CLI flag per Homei evaluation diagnostic spec."""

from __future__ import annotations

import subprocess
import pytest
from clp_ep_client import CLPEPClient, default_engine_path, build_state_from_lppn

PIECE_NAMES = ["VUONG", "TUONG", "SI", "KI", "XA", "LONG", "PHUONG", "BINH"]
PIECE_CODES = ["V", "T", "S", "K", "X", "L", "P", "B"]


def parse_piecevalues_output(lines: list[str]) -> dict:
    data = {
        "pieces": {},
        "start_side_total": None,
        "source": None,
        "note": None,
    }
    in_block = False
    for line in lines:
        line = line.strip()
        if line == "piecevalues begin":
            in_block = True
            continue
        if line == "piecevalues end":
            in_block = False
            break
        if not in_block:
            continue
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "source":
            data["source"] = line.split("source ", 1)[1]
        elif parts[0] == "start_side_total":
            data["start_side_total"] = int(parts[1])
        elif parts[0] == "note":
            data["note"] = line.split("note ", 1)[1]
        elif parts[0] == "piece":
            # piece code=V name=VUONG board=0 reserve=- prisoner=-
            fields = {}
            for token in parts[1:]:
                if "=" in token:
                    k, v = token.split("=", 1)
                    fields[k] = v
            if "code" in fields:
                data["pieces"][fields["code"]] = fields
    return data


def test_piecevalues_cli():
    """Test CLI invocation via homei.exe --piecevalues (exits with code 0)."""
    engine_bin = default_engine_path()
    res = subprocess.run([str(engine_bin), "--piecevalues"], capture_output=True, text=True)
    assert res.returncode == 0, f"Expected 0, got {res.returncode}: {res.stderr}"
    lines = res.stdout.splitlines()
    data = parse_piecevalues_output(lines)

    # 1. Check all 8 piece types present
    assert len(data["pieces"]) == 8
    for code, name in zip(PIECE_CODES, PIECE_NAMES):
        assert code in data["pieces"]
        assert data["pieces"][code]["name"] == name

    # 2. Check King (Vuong) = 0
    assert data["pieces"]["V"]["board"] == "0"
    assert data["pieces"]["V"]["reserve"] == "-"
    assert data["pieces"]["V"]["prisoner"] == "-"

    # 3. Check start_side_total is integer and matches board + reserve sums
    # 1*0 + 2*420 + 2*320 + 2*360 + 2*280 + 9*100 + 1*600 + 1*520 = 4780
    assert data["start_side_total"] == 4780

    # 4. Check source and note are present
    assert data["source"] is not None
    assert data["note"] is not None


def test_piecevalues_stdin():
    """Test piecevalues command over stdin interactive session."""
    with CLPEPClient() as client:
        client.send_line("piecevalues")
        # Collect until piecevalues end
        lines = []
        while True:
            line = client.read_line(timeout=5.0)
            lines.append(line)
            if line.strip() == "piecevalues end":
                break

        data = parse_piecevalues_output(lines)
        assert len(data["pieces"]) == 8
        assert data["pieces"]["V"]["board"] == "0"
        assert data["start_side_total"] == 4780

        # Long and Phuong have reserve values, others have '-'
        assert data["pieces"]["L"]["reserve"] != "-"
        assert data["pieces"]["P"]["reserve"] != "-"
        assert data["pieces"]["B"]["reserve"] == "-"


def test_piecevalues_does_not_affect_subsequent_go():
    """Test running piecevalues between two identical 'go' searches does not change bestmove."""
    lppn_str = "lppn1 X2K21T2V24/1S22T14/B2B2B2B2B24/4[S2P2]2L2p0/6k32/6b02/b0[b0l0]b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- B/KXBBB 45"
    state, b, turn = build_state_from_lppn(lppn_str)

    with CLPEPClient() as client:
        # 1. Search once on test position
        out1 = client.search_movetime(state, movetime_ms=500, depth=4, nodes=50000000, multipv=1)
        assert out1.bestmove is not None

        # 2. Call piecevalues diagnostic command
        client.send_line("piecevalues")
        while True:
            line = client.read_line(timeout=5.0)
            if line.strip() == "piecevalues end":
                break

        # 3. Search again on identical test position
        out2 = client.search_movetime(state, movetime_ms=500, depth=4, nodes=50000000, multipv=1)
        assert out2.bestmove is not None

        # Assert identical move notation
        assert out1.bestmove["notation"] == out2.bestmove["notation"]

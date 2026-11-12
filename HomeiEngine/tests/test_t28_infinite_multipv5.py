"""Test verifying T28 (ply = 119) under MultiPV = 5 and go infinite mode."""

import base64
import json
from pathlib import Path
import sys
import time

HOMEI_ROOT = Path(__file__).resolve().parents[2]
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))

TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

from clp_ep_client import CLPEPClient, build_state_from_lppn
from test_mate_correctness import T28_LPPN


def test_t28_infinite_multipv5():
    exe = str(HOMEI_ROOT / "HomeiEngine" / "build" / "Engine realese" / "Homei_v2-2.exe")
    st, b, t = build_state_from_lppn(T28_LPPN)

    with CLPEPClient(executable=exe) as client:
        client.set_position_state(st)
        client.set_option("MultiPV", 5)

        client.send_line("go id 1 infinite")
        started = client.read_line(timeout=5.0)
        assert started == "searchstarted id 1"

        # Wait 800 ms per requirement
        time.sleep(0.8)
        client.send_line("stop id 1")

        infos = []
        bestmove = None
        while True:
            line = client.read_line(timeout=10.0)
            if line.startswith("info id 1 info64 "):
                token = line.split(" ", 4)[4]
                cleaned = token.strip()
                pad = "=" * ((-len(cleaned)) % 4)
                obj = json.loads(base64.urlsafe_b64decode(cleaned + pad).decode("utf-8"))
                infos.append(obj)
            elif line.startswith("bestmove "):
                bestmove = line
                break

        by_rank = {}
        for info in infos:
            by_rank[info["multipv"]] = info

        print("Discovered ranks:", sorted(by_rank.keys()))
        assert 1 in by_rank, "multipv 1 missing"
        rank1 = by_rank[1]

        # 1. multipv 1 outputs score mate 15 with first move K*a3<
        assert rank1["score"]["kind"] == "mate", f"Rank 1 expected mate, got {rank1['score']}"
        assert rank1["score"]["value"] == 15, f"Rank 1 expected mate 15, got {rank1['score']['value']}"
        first_move_rank1 = rank1["pv"][0]["notation"]
        assert first_move_rank1 == "K*a3<", f"Rank 1 expected K*a3<, got {first_move_rank1}"

        # 2. multipv 2..5 exist and provide 4 distinct alternative moves
        for r in range(2, 6):
            assert r in by_rank, f"Rank {r} missing from infos"

        top5_moves = [by_rank[r]["pv"][0]["notation"] for r in range(1, 6)]
        print("Top 5 moves across multipv 1..5:", top5_moves)
        assert len(set(top5_moves)) == 5, f"Top 5 moves not pairwise distinct: {top5_moves}"

        # Also check bestmove matches rank 1
        bm_token = bestmove.split(" ")[4]
        bm_pad = "=" * ((-len(bm_token)) % 4)
        bm_obj = json.loads(base64.urlsafe_b64decode(bm_token + bm_pad).decode("utf-8"))
        assert bm_obj["notation"] == "K*a3<", f"Bestmove expected K*a3<, got {bm_obj['notation']}"
        print("T28 Infinite MultiPV 5 Verification: PASS 100%")


if __name__ == "__main__":
    test_t28_infinite_multipv5()

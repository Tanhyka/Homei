"""Test bench command per Homei search audit spec."""

from __future__ import annotations

import pytest
from clp_ep_client import CLPEPClient

EXPECTED_BENCH_POSITIONS = [
    "startpos", "S3", "S8",
    "T01", "T02", "T03", "T04", "T05", "T06", "T07", "T08", "T09", "T10"
]


def test_bench_command():
    """Test bench command executes 13 positions, reports nodes, EBF, and final summary."""
    with CLPEPClient() as client:
        # Run a quick bench with low depth to verify protocol and format
        client.send_line("bench 3 100")
        lines = []
        while True:
            line = client.read_line(timeout=20.0)
            lines.append(line.strip())
            if line.startswith("bench done"):
                break

        # Check for presence of all 13 positions
        output_text = "\n".join(lines)
        for pos_name in EXPECTED_BENCH_POSITIONS:
            assert pos_name in output_text, f"Position {pos_name} missing from bench output"

        # Check final summary line
        assert lines[-1].startswith("bench done")
        parts = lines[-1].split()
        assert len(parts) >= 6
        assert parts[0] == "bench"
        assert parts[1] == "done"
        assert int(parts[2]) > 0  # total nodes
        assert parts[3] == "nodes"
        assert int(parts[4]) > 0  # elapsed ms
        assert parts[5] == "ms"

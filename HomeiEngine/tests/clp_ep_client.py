"""CLP-EP v1 subprocess client and state builder for HomeiEngine pytest suite."""

from __future__ import annotations

import base64
import copy
from dataclasses import dataclass, field
import json
from pathlib import Path
import subprocess
import sys
import threading
import time
from typing import Any, Dict, List, Optional, Tuple

HOMEI_ROOT = Path(__file__).resolve().parents[2]
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))

from ref.clp import board as clp_board
from ref.clp import core_bridge
from ref.clp import lppn as clp_lppn
from ref.clp import pieces as clp_pieces
from ref.clp import rules as clp_rules


def b64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def b64url_decode(token: str) -> bytes:
    cleaned = token.strip()
    if not cleaned:
        raise ValueError("Empty base64url token")
    pad = "=" * ((-len(cleaned)) % 4)
    return base64.urlsafe_b64decode(cleaned + pad)


def encode_state64(state_dict: dict) -> str:
    raw = json.dumps(state_dict, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return b64url_encode(raw)


def decode_state64(token: str) -> dict:
    return json.loads(b64url_decode(token).decode("utf-8"))


def build_state_from_lppn(lppn_str: str) -> Tuple[dict, clp_board.Board, clp_pieces.Player]:
    """Builds a clp.state/1 dictionary from LPPN exactly like CoreBridge.importLppnStrict."""
    decoded = clp_lppn.from_lppn(lppn_str, strict=True)
    pos_dict = core_bridge._encode_position_py(decoded.board, decoded.turn)
    state = {
        "notation": None,
        "ply": decoded.ply,
        "position": pos_dict,
        "repetition": {
            "complete": True,
            "entries": [{"count": 1, "position": copy.deepcopy(pos_dict)}],
        },
        "result": {"status": "ongoing"},
        "schema": "clp.state/1",
    }
    core_bridge._adjudicate_state_py(state)
    return state, decoded.board, decoded.turn


def build_state_from_lppn_and_moves(
    lppn_str: str, move_notations: List[str]
) -> Tuple[dict, clp_board.Board, clp_pieces.Player]:
    """Builds a clp.state/1 dictionary by replaying canonical moves from a base LPPN."""
    state, b, turn = build_state_from_lppn(lppn_str)
    for not_str in move_notations:
        legal = clp_rules.legal_moves(b, turn)
        matched = [m for m in legal if m.canonical_notation(b) == not_str]
        if len(matched) != 1:
            raise ValueError(f"Move {not_str!r} not uniquely legal in {clp_lppn.to_lppn(b, turn, ply=state['ply'])}")
        mv = matched[0]
        clp_rules.apply_move(b, turn, mv)
        turn = clp_rules.other(turn)
        next_pos = core_bridge._encode_position_py(b, turn)
        state["position"] = next_pos
        state["ply"] = int(state["ply"]) + 1
        next_id = core_bridge._position_identity_py(next_pos)
        entries = state["repetition"]["entries"]
        found = False
        for entry in entries:
            if core_bridge._position_identity_py(entry["position"]) == next_id:
                entry["count"] += 1
                found = True
                break
        if not found:
            entries.append({"count": 1, "position": copy.deepcopy(next_pos)})
        core_bridge._adjudicate_state_py(state)
    return state, b, turn


def default_engine_path() -> Path:
    build_dir = Path(__file__).resolve().parents[1] / "build"
    for name in ("clp_core_cli.exe", "homei.exe", "clp_core_cli", "homei"):
        candidate = build_dir / name
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"Engine executable not found in {build_dir}")


@dataclass
class SearchOutput:
    req_id: int
    bestmove_raw: str
    bestmove: Optional[dict] = None
    infos: List[dict] = field(default_factory=list)
    strings: List[str] = field(default_factory=list)
    elapsed_ms: float = 0.0


class CLPEPClient:
    """Synchronous/asynchronous subprocess client for CLP-EP v1 testing."""

    def __init__(self, executable: Optional[Path] = None):
        self.executable = Path(executable) if executable else default_engine_path()
        self.proc: Optional[subprocess.Popen] = None
        self.handshake_lines: List[str] = []
        self._next_id = 0

    def start(self, do_handshake: bool = True) -> None:
        self.proc = subprocess.Popen(
            [str(self.executable)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        if do_handshake:
            self.handshake()

    def close(self) -> None:
        if self.proc is None:
            return
        if self.proc.poll() is None:
            try:
                self.send_line("quit")
                self.proc.wait(timeout=1.0)
            except Exception:
                self.proc.kill()
                self.proc.wait(timeout=1.0)
        self.proc = None

    def __enter__(self) -> "CLPEPClient":
        self.start(do_handshake=True)
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def send_line(self, line: str) -> None:
        assert self.proc is not None and self.proc.stdin is not None
        self.proc.stdin.write((line + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def read_line(self, timeout: float = 35.0) -> str:
        assert self.proc is not None and self.proc.stdout is not None
        result: List[bytes] = []
        err: List[BaseException] = []

        def _reader():
            try:
                line = self.proc.stdout.readline()
                result.append(line)
            except BaseException as ex:
                err.append(ex)

        t = threading.Thread(target=_reader, daemon=True)
        t.start()
        t.join(timeout=timeout)
        if t.is_alive():
            raise TimeoutError(f"Timed out waiting for line from engine after {timeout}s")
        if err:
            raise err[0]
        if not result or not result[0]:
            raise EOFError("Engine stdout closed unexpectedly")
        return result[0].decode("utf-8", errors="replace").strip()

    def handshake(self) -> List[str]:
        self.send_line("clp 1")
        lines = []
        while True:
            line = self.read_line(timeout=5.0)
            lines.append(line)
            if line == "clpok":
                break
        self.handshake_lines = lines
        self.send_line("isready")
        ready = self.read_line(timeout=5.0)
        if ready != "readyok":
            raise RuntimeError(f"Expected readyok, got {ready!r}")
        return lines

    def set_position_state(self, state_dict: dict) -> str:
        b64 = encode_state64(state_dict)
        self.send_line(f"position state64 {b64}")
        resp = self.read_line(timeout=5.0)
        if not resp.startswith("positionok "):
            raise RuntimeError(f"position state64 failed: {resp}")
        return resp

    def set_position_lppn(self, lppn_str: str) -> str:
        self.send_line(f"position lppn {lppn_str}")
        resp = self.read_line(timeout=5.0)
        if not resp.startswith("positionok "):
            raise RuntimeError(f"position lppn failed: {resp}")
        return resp

    def set_option(self, name: str, value: int) -> str:
        self.send_line(f"setoption name {name} value {value}")
        resp = self.read_line(timeout=5.0)
        expected = f"optionok name {name} value {value}"
        if resp != expected:
            raise RuntimeError(f"Expected {expected!r}, got {resp!r}")
        return resp

    def search_movetime(
        self,
        state_dict: dict,
        movetime_ms: int = 5000,
        depth: int = 32,
        nodes: int = 50000000,
        multipv: int = 1,
    ) -> SearchOutput:
        self.set_position_state(state_dict)
        self.set_option("Depth", depth)
        self.set_option("Nodes", nodes)
        self.set_option("MultiPV", multipv)

        self._next_id += 1
        req_id = self._next_id
        t0 = time.perf_counter()
        self.send_line(f"go id {req_id} movetime {movetime_ms}")

        infos: List[dict] = []
        strings: List[str] = []
        timeout_s = max(10.0, (movetime_ms / 1000.0) + 10.0)

        while True:
            line = self.read_line(timeout=timeout_s)
            if line.startswith(f"info id {req_id} info64 "):
                token = line.split(" ", 4)[4]
                info_obj = json.loads(b64url_decode(token).decode("utf-8"))
                infos.append(info_obj)
            elif line.startswith(f"info id {req_id} string64 "):
                token = line.split(" ", 4)[4]
                msg_obj = json.loads(b64url_decode(token).decode("utf-8"))
                strings.append(msg_obj.get("message", ""))
            elif line.startswith(f"bestmove id {req_id} "):
                elapsed_ms = (time.perf_counter() - t0) * 1000.0
                parts = line.split(" ")
                bestmove_obj = None
                if len(parts) == 5 and parts[3] == "move64":
                    bestmove_obj = json.loads(b64url_decode(parts[4]).decode("utf-8"))
                return SearchOutput(
                    req_id=req_id,
                    bestmove_raw=line,
                    bestmove=bestmove_obj,
                    infos=infos,
                    strings=strings,
                    elapsed_ms=elapsed_ms,
                )
            elif line.startswith("error "):
                raise RuntimeError(f"Engine error during search: {line}")

    def get_legal_notations(self) -> List[str]:
        self.send_line("legal")
        line = self.read_line(timeout=5.0)
        if not line.startswith("legal"):
            raise RuntimeError(f"Unexpected response to legal: {line!r}")
        parts = line.split(" ")
        count = int(parts[1])
        moves = parts[2:] if count > 0 else []
        assert len(moves) == count
        return moves

    def get_legal_dicts(self) -> List[dict]:
        self.send_line("legal64")
        line = self.read_line(timeout=5.0)
        if not line.startswith("legal64 "):
            raise RuntimeError(f"Unexpected response to legal64: {line!r}")
        parts = line.split(" ")
        count = int(parts[1])
        arr = json.loads(b64url_decode(parts[2]).decode("utf-8"))
        assert len(arr) == count
        return arr

    def get_perft(self, depth: int) -> int:
        self.send_line(f"perft {depth}")
        line = self.read_line(timeout=30.0)
        parts = line.split(" ")
        if len(parts) != 3 or parts[0] != "perft" or int(parts[1]) != depth:
            raise RuntimeError(f"Unexpected perft response: {line!r}")
        return int(parts[2])

    def get_lppn(self) -> str:
        self.send_line("lppn")
        line = self.read_line(timeout=5.0)
        if not line.startswith("lppn "):
            raise RuntimeError(f"Unexpected lppn response: {line!r}")
        return line[5:]

    def get_state_dict(self) -> dict:
        self.send_line("state64")
        line = self.read_line(timeout=5.0)
        if not line.startswith("state64 "):
            raise RuntimeError(f"Unexpected state64 response: {line!r}")
        return decode_state64(line[8:])

    def apply_notation(self, notation: str) -> str:
        self.send_line(f"apply {notation}")
        return self.read_line(timeout=5.0)

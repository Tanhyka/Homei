"""CoreBridge: Python port of CLP_GUI_TRAINING/src/core_bridge.h & .cpp.

Uses the compiled C++ cpp-CLP DLL (clp_rules_service.dll) when available for
high-speed native move generation, state encoding, and PV verification, with an
exact pure-Python Ryuugin-compatible fallback.
"""

from __future__ import annotations

import base64
import copy
import json
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from .board import Board, SIZE
from .lppn import from_lppn
from .native import native_core
from .pieces import PIECE_CODE, Piece, PieceType, Player
from .tree import MoveNode
from . import rules


@dataclass
class DisplayCell:
    piece: str = ""
    cam: bool = True
    layers: int = 0
    facing: int = 0


@dataclass
class DisplayBoard:
    valid: bool = False
    camTurn: bool = True
    cells: List[DisplayCell] = field(default_factory=lambda: [DisplayCell() for _ in range(81)])


def _set_error(error_out: Optional[List[str]], message: str) -> None:
    if isinstance(error_out, list):
        error_out.clear()
        error_out.append(message)


def _encode_position_py(board: Board, turn: Player) -> dict:
    cells = []
    for r in range(SIZE):
        for c in range(SIZE):
            st = board.sq(r, c).stack
            cells.append(
                [
                    {
                        "facing": int(p.facing),
                        "owner": p.owner.name,
                        "type": p.ptype.name,
                    }
                    for p in st
                ]
            )
    return {
        "board": cells,
        "prisoners": {
            "CAM": [pt.name for pt in board.prisoners[Player.CAM]],
            "XANH": [pt.name for pt in board.prisoners[Player.XANH]],
        },
        "reserve": {
            "CAM": [pt.name for pt in board.reserve[Player.CAM]],
            "XANH": [pt.name for pt in board.reserve[Player.XANH]],
        },
        "to_move": turn.name,
    }


def _decode_position_py(pos_dict: dict) -> Tuple[Board, Player]:
    if not isinstance(pos_dict, dict) or set(pos_dict.keys()) != {"board", "to_move", "reserve", "prisoners"}:
        raise ValueError("Invalid CLP position fields")
    board_arr = pos_dict["board"]
    if not isinstance(board_arr, list) or len(board_arr) != 81:
        raise ValueError("Expected 81 squares")
    b = Board()
    b.reserve = {Player.CAM: [], Player.XANH: []}
    b.prisoners = {Player.CAM: [], Player.XANH: []}
    turn = Player[pos_dict["to_move"]]
    for i, cell in enumerate(board_arr):
        if not isinstance(cell, list):
            raise ValueError("Invalid cell stack")
        r, c = divmod(i, 9)
        for p in cell:
            if not isinstance(p, dict) or set(p.keys()) != {"type", "owner", "facing"}:
                raise ValueError("Invalid piece fields")
            b.sq(r, c).stack.append(
                Piece(PieceType[p["type"]], Player[p["owner"]], int(p["facing"]))
            )
    for hand_name in ("reserve", "prisoners"):
        hand = pos_dict[hand_name]
        if not isinstance(hand, dict) or set(hand.keys()) != {"CAM", "XANH"}:
            raise ValueError("Invalid hand fields")
        setattr(
            b,
            hand_name,
            {
                Player.CAM: [PieceType[t] for t in hand["CAM"]],
                Player.XANH: [PieceType[t] for t in hand["XANH"]],
            },
        )
    return b, turn


def _position_identity_py(pos_dict: dict) -> str:
    return json.dumps(pos_dict, sort_keys=True, separators=(",", ":"))


def _encode_move_py(move: rules.Move, notation=None) -> dict:
    return {
        "dst": list(move.dst) if move.dst is not None else None,
        "facing": int(move.facing) if move.facing is not None else None,
        "kind": move.kind,
        "notation": notation,
        "ptype": move.ptype.name if move.ptype is not None else "",
        "rotate_after": int(move.rotate_after),
        "schema": "clp.move/1",
        "src": list(move.src) if move.src is not None else None,
    }


def _decode_move_py(move_dict: dict) -> rules.Move:
    if not isinstance(move_dict, dict):
        raise ValueError("Move must be a JSON object")
    required = {"schema", "kind", "src", "dst", "ptype", "facing", "rotate_after", "notation"}
    if set(move_dict.keys()) != required or move_dict.get("schema") != "clp.move/1":
        raise ValueError("Invalid move schema")
    kind = move_dict["kind"]
    if kind not in ("single", "team", "rotate", "drop"):
        raise ValueError("Invalid move kind")

    def sq(val):
        if val is None:
            return None
        if not isinstance(val, list) or len(val) != 2 or any(type(x) is not int or not (0 <= x < 9) for x in val):
            raise ValueError("Invalid square")
        return (val[0], val[1])

    src = sq(move_dict["src"])
    dst = sq(move_dict["dst"])
    ptype = PieceType[move_dict["ptype"]]
    facing = move_dict["facing"]
    if facing is not None and (type(facing) is not int or not (0 <= facing < 4)):
        raise ValueError("Invalid facing")
    rot = move_dict["rotate_after"]
    if type(rot) is not int or rot not in (0, 1, 3):
        raise ValueError("Invalid rotation")
    return rules.Move(kind=kind, src=src, dst=dst, ptype=ptype, facing=facing, rotate_after=rot)


def _all_legal_moves_lead_to_rep3_py(board: Board, turn: Player, entries: List[dict]) -> bool:
    """Điều 5 (Thiên Nhật Hòa): True iff every legal move from (board, turn) results in count >= 3."""
    legal = rules.legal_moves(board, turn)
    if not legal:
        return False
    counts_ge2 = {
        _position_identity_py(e["position"])
        for e in entries
        if int(e.get("count", 0)) >= 2
    }
    if not counts_ge2:
        return False
    next_turn = rules.other(turn)
    for cand in legal:
        sim = board.clone()
        rules.apply_move(sim, turn, cand)
        cand_id = _position_identity_py(_encode_position_py(sim, next_turn))
        if cand_id not in counts_ge2:
            return False
    return True


def _adjudicate_state_py(state: dict, unavoidable_rep3: bool = False) -> None:
    pos_dict = state["position"]
    cur_id = _position_identity_py(pos_dict)
    entries = state["repetition"]["entries"]
    matched = None
    for entry in entries:
        if _position_identity_py(entry["position"]) == cur_id:
            matched = entry
            break
    if matched is None:
        raise ValueError("Current history missing")
    board, turn = _decode_position_py(pos_dict)
    if matched["count"] >= 3:
        if unavoidable_rep3:
            state["result"] = {
                "loser": None,
                "reason": "repetition3_draw",
                "status": "terminal",
                "winner": None,
            }
            return
        loser = rules.other(turn)
        state["result"] = {
            "loser": loser.name,
            "reason": "repetition3",
            "status": "terminal",
            "winner": turn.name,
        }
        return
    legal = rules.legal_moves(board, turn)
    if not legal:
        in_chk = rules.is_in_check(board, turn)
        winner = rules.other(turn)
        state["result"] = {
            "loser": turn.name,
            "reason": "checkmate" if in_chk else "pat",
            "status": "terminal",
            "winner": winner.name,
        }
    else:
        state["result"] = {"status": "ongoing"}


class CoreBridge:
    """Authoritative rules & game history bridge matching CLP_GUI_TRAINING CoreBridge."""

    def __init__(self):
        self._native = native_core()
        self.root_node: Optional[MoveNode] = None
        self.current_node: Optional[MoveNode] = None
        self._states: List[dict] = []
        self._names: List[str] = []
        self._comment_moves: List[dict] = []
        self._selected: int = 0
        self._trusted_ancestry: bool = False
        self._legal_cache: Dict[int, Tuple[List[dict], List[str]]] = {}
        self.last_error: str = ""
        self.reset()

    def _active_line_nodes(self) -> List[MoveNode]:
        """Returns the full sequence of nodes along the active branch:
        from root down to self.current_node, and continuing down children[0] to leaf."""
        if self.current_node is None:
            return [self.root_node] if self.root_node is not None else []
        path = self.current_node.path_from_root()
        curr = self.current_node
        continuation = []
        while curr and curr.children:
            curr = curr.children[0]
            continuation.append(curr)
        return path + continuation

    def _sync_active_path(self) -> None:
        """Synchronizes flat structures (_states, _names, _comment_moves, _selected)
        to match the active line passing through current_node."""
        if self.current_node is None:
            self._states = []
            self._names = []
            self._comment_moves = []
            self._selected = 0
            return
        active_line = self._active_line_nodes()
        self._states = [n.state for n in active_line if n.state is not None]
        self._names = [n.notation_text for n in active_line[1:]]
        self._comment_moves = [n.move for n in active_line[1:] if n.move is not None]
        path_to_curr = self.current_node.path_from_root()
        self._selected = len(path_to_curr) - 1

    def containsNode(self, node: Optional[MoveNode]) -> bool:
        if node is None or self.root_node is None:
            return False
        return any(n is node for n in self.root_node.all_nodes())

    def selectNode(self, node: MoveNode, error: Optional[List[str]] = None) -> bool:
        if not self.containsNode(node):
            self.last_error = "Node not in current move tree"
            _set_error(error, self.last_error)
            return False
        self.current_node = node
        self._sync_active_path()
        self.last_error = ""
        return True

    def promoteNode(self, node: MoveNode, error: Optional[List[str]] = None) -> bool:
        if not self.containsNode(node):
            self.last_error = "Node not in current move tree"
            _set_error(error, self.last_error)
            return False
        node.promote()
        self.current_node = node
        self._sync_active_path()
        self.last_error = ""
        return True

    def deleteBranch(self, node: MoveNode, error: Optional[List[str]] = None) -> bool:
        if not self.containsNode(node):
            self.last_error = "Node not in current move tree"
            _set_error(error, self.last_error)
            return False
        if node is self.root_node or node.parent is None:
            self.last_error = "Cannot delete root node"
            _set_error(error, self.last_error)
            return False
        parent = node.parent
        node.delete_branch()
        if not self.containsNode(self.current_node):
            self.current_node = parent
        self._sync_active_path()
        self.last_error = ""
        return True

    def linked(self) -> bool:
        return True

    def native_linked(self) -> bool:
        return bool(self._native and self._native.available)

    def status(self) -> str:
        if self.native_linked():
            return "Ryuugin cpp-CLP rules linked"
        return "Ryuugin cpp-CLP rules linked (Python Core)"

    def _selected_state(self) -> Optional[dict]:
        if not self._states or not (0 <= self._selected < len(self._states)):
            return None
        return self._states[self._selected]

    def _initial_state(self) -> dict:
        if self.native_linked():
            return self._native.call("initial")
        b = Board()
        b.setup_initial()
        pos = _encode_position_py(b, Player.CAM)
        return {
            "notation": None,
            "ply": 0,
            "position": pos,
            "repetition": {
                "complete": True,
                "entries": [{"count": 1, "position": copy.deepcopy(pos)}],
            },
            "result": {"status": "ongoing"},
            "schema": "clp.state/1",
        }

    def _compute_legal_and_names(self, state: dict) -> Tuple[List[dict], List[str]]:
        if state.get("result", {}).get("status") == "terminal":
            return [], []
        if self.native_linked():
            try:
                moves = self._native.call("legal", state=state)
            except ValueError as ex:
                if "no legal moves" in str(ex):
                    return [], []
                raise
            pos = state["position"]
            names = [self._native.call("format", position=pos, move=m) for m in moves]
            return moves, names
        board, turn = _decode_position_py(state["position"])
        py_moves = rules.legal_moves(board, turn)
        moves = [_encode_move_py(m, None) for m in py_moves]
        names = [m.canonical_notation(board) for m in py_moves]
        return moves, names

    def _get_cached_legal(self) -> Tuple[List[dict], List[str]]:
        state = self._selected_state()
        if state is None:
            return [], []
        if self._selected not in self._legal_cache:
            self._legal_cache[self._selected] = self._compute_legal_and_names(state)
        return self._legal_cache[self._selected]

    def _apply_move_to_state(self, state: dict, move_dict: dict) -> dict:
        if self.native_linked():
            return self._native.call("apply", state=state, move=move_dict)
        if state.get("result", {}).get("status") == "terminal":
            raise ValueError("game terminal")
        board, turn = _decode_position_py(state["position"])
        move = _decode_move_py(move_dict)
        legal = rules.legal_moves(board, turn)
        if not any(
            m.kind == move.kind
            and m.src == move.src
            and m.dst == move.dst
            and m.ptype == move.ptype
            and m.facing == move.facing
            and m.rotate_after == move.rotate_after
            for m in legal
        ):
            raise ValueError("illegal move")
        board_before = board.clone()
        rules.apply_move(board, turn, move)
        next_turn = rules.other(turn)
        next_pos = _encode_position_py(board, next_turn)
        next_state = copy.deepcopy(state)
        next_state["position"] = next_pos
        next_state["ply"] = int(state["ply"]) + 1
        next_id = _position_identity_py(next_pos)
        entries = next_state["repetition"]["entries"]
        found = False
        new_count = 1
        for entry in entries:
            if _position_identity_py(entry["position"]) == next_id:
                entry["count"] += 1
                new_count = entry["count"]
                found = True
                break
        if not found:
            entries.append({"count": 1, "position": copy.deepcopy(next_pos)})
        unavoidable = False
        if new_count >= 3:
            unavoidable = _all_legal_moves_lead_to_rep3_py(board_before, turn, state["repetition"]["entries"])
        _adjudicate_state_py(next_state, unavoidable_rep3=unavoidable)
        return next_state

    def reset(self, error: Optional[List[str]] = None) -> bool:
        try:
            init_state = self._initial_state()
            self.root_node = MoveNode(
                move=None,
                parent=None,
                notation_text="0. [Start]",
                side="CAM",
                ply=0,
                state=init_state,
            )
            self.current_node = self.root_node
            self._sync_active_path()
            self._trusted_ancestry = True
            self._legal_cache.clear()
            self.last_error = ""
            return True
        except Exception as ex:
            self.root_node = None
            self.current_node = None
            self._states = []
            self._names = []
            self._comment_moves = []
            self._selected = 0
            self._trusted_ancestry = False
            self._legal_cache.clear()
            self.last_error = str(ex)
            _set_error(error, self.last_error)
            return False

    def importLppnStrict(self, text: str, error: Optional[List[str]] = None) -> bool:
        try:
            decoded = from_lppn(text, strict=True)
            pos_dict = _encode_position_py(decoded.board, decoded.turn)
            if self.native_linked():
                state = self._native.call("base", position=pos_dict)
                state["ply"] = decoded.ply
            else:
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
                _adjudicate_state_py(state)
            self.root_node = MoveNode(
                move=None,
                parent=None,
                notation_text=f"{decoded.ply}. [LPPN]",
                side=decoded.turn.name,
                ply=decoded.ply,
                state=state,
            )
            self.current_node = self.root_node
            self._sync_active_path()
            self._trusted_ancestry = False
            self._legal_cache.clear()
            self.last_error = ""
            return True
        except Exception as ex:
            self.last_error = str(ex)
            _set_error(error, self.last_error)
            return False

    def loadHistoryStates(
        self,
        states: List[dict],
        names: List[str],
        comment_moves: List[dict],
        selected_ply: Optional[int] = None,
        ending: Optional[str] = None,
    ) -> bool:
        """Fast bulk loader for complete game histories without per-ply IPC roundtrips."""
        if not states:
            return False
        # Adjudicate the final position (mate/pat/repetition) so the core never
        # reports "ongoing" for a position without legal moves.
        try:
            last = states[-1]
            if self.native_linked():
                last = self._native.call("status", state=last)
                if last.get("result", {}).get("status") != "terminal" and ending in ("resign", "mate"):
                    last = self._native.call("resign", state=last)
            else:
                _adjudicate_state_py(last)
            states[-1] = last
        except Exception as ex:
            self.last_error = str(ex)

        turn_side = "CAM"
        if states and "position" in states[0]:
            turn_side = states[0]["position"].get("to_move", "CAM")
        self.root_node = MoveNode(
            move=None,
            parent=None,
            notation_text="0. [Start]",
            side=turn_side,
            ply=0,
            state=states[0],
        )
        curr = self.root_node
        for idx in range(1, len(states)):
            mv = comment_moves[idx - 1] if idx - 1 < len(comment_moves) else None
            txt = names[idx - 1] if idx - 1 < len(names) else ""
            prev_st = states[idx - 1]
            side_moved = prev_st.get("position", {}).get("to_move", "CAM") if "position" in prev_st else ""
            curr = curr.add_child(
                move=mv,
                state=states[idx],
                notation_text=txt,
                side=side_moved,
                ply=idx,
            )

        target_idx = selected_ply if selected_ply is not None else (len(states) - 1)
        path = self.root_node.all_nodes()
        if 0 <= target_idx < len(path):
            self.current_node = path[target_idx]
        else:
            self.current_node = curr

        self._sync_active_path()
        self._trusted_ancestry = True
        self._legal_cache.clear()
        self.last_error = ""
        return True

    def playLegalMove(self, row: int, error: Optional[List[str]] = None) -> bool:
        try:
            cur = self._selected_state()
            if cur is None:
                raise RuntimeError("No position")
            legal_moves, legal_names = self._get_cached_legal()
            if row < 0 or row >= len(legal_moves):
                raise RuntimeError("Invalid legal move index")
            move_dict = copy.deepcopy(legal_moves[row])
            move_name = legal_names[row]
            next_state = self._apply_move_to_state(cur, move_dict)

            side_moved = cur.get("position", {}).get("to_move", "CAM") if "position" in cur else ""
            if self.current_node is None:
                self.reset()
            child_node = self.current_node.add_child(
                move=move_dict,
                state=next_state,
                notation_text=move_name,
                side=side_moved,
                ply=self.current_node.ply + 1,
            )
            self.current_node = child_node
            self._sync_active_path()
            self._legal_cache.clear()
            self.last_error = ""
            return True
        except Exception as ex:
            self.last_error = str(ex)
            _set_error(error, self.last_error)
            return False

    def selectPly(self, row: int, error: Optional[List[str]] = None) -> bool:
        active_line = self._active_line_nodes()
        if 0 <= row < len(active_line):
            self.current_node = active_line[row]
            self._sync_active_path()
            self.last_error = ""
            return True
        self.last_error = "Invalid history selection"
        _set_error(error, self.last_error)
        return False

    def playEngineMove(self, move_json: bytes | str | dict, error: Optional[List[str]] = None) -> bool:
        try:
            cur = self._selected_state()
            if cur is None or not self._trusted_ancestry:
                raise RuntimeError("No trusted position")
            if isinstance(move_json, (bytes, bytearray)):
                move_obj = json.loads(move_json.decode("utf-8"))
            elif isinstance(move_json, str):
                move_obj = json.loads(move_json)
            elif isinstance(move_json, dict):
                move_obj = copy.deepcopy(move_json)
            else:
                raise RuntimeError("Invalid move payload")

            # Validate move schema & fields
            _decode_move_py(move_obj)
            legal_moves, _ = self._get_cached_legal()

            def _canonical_key(m: dict):
                return (
                    m.get("schema"),
                    m.get("kind"),
                    m.get("src"),
                    m.get("dst"),
                    m.get("ptype"),
                    m.get("facing"),
                    m.get("rotate_after"),
                    m.get("notation"),
                )

            target_key = _canonical_key(move_obj)
            for idx, candidate in enumerate(legal_moves):
                if _canonical_key(candidate) == target_key:
                    return self.playLegalMove(idx, error)

            # Fallback if engine included notation while core legal move has notation=None
            def _key_no_notation(m: dict):
                return (
                    m.get("schema"),
                    m.get("kind"),
                    m.get("src"),
                    m.get("dst"),
                    m.get("ptype"),
                    m.get("facing"),
                    m.get("rotate_after"),
                )

            if move_obj.get("notation") is None:
                for idx, candidate in enumerate(legal_moves):
                    if _key_no_notation(candidate) == _key_no_notation(move_obj):
                        return self.playLegalMove(idx, error)

            raise RuntimeError("Engine returned a move not in Core legal moves")
        except Exception as ex:
            self.last_error = str(ex)
            _set_error(error, self.last_error)
            return False

    def legalMoveNames(self) -> List[str]:
        try:
            cur = self._selected_state()
            if cur is None or cur.get("result", {}).get("status") == "terminal":
                return []
            _, names = self._get_cached_legal()
            return list(names)
        except Exception:
            return []

    def policyPosition(self) -> dict:
        cur = self._selected_state()
        return copy.deepcopy(cur) if cur is not None else {}

    def policyMoves(self) -> List[dict]:
        cur = self._selected_state()
        if cur is None or cur.get("result", {}).get("status") == "terminal":
            return []
        moves, _ = self._get_cached_legal()
        return copy.deepcopy(moves)

    def commentIdentity(self) -> dict:
        if self._trusted_ancestry and 0 < self._selected <= len(self._comment_moves):
            def _clean_state(st: dict) -> dict:
                d = copy.deepcopy(st)
                d.pop("notation", None)
                return d

            path = [copy.deepcopy(self._comment_moves[i]) for i in range(self._selected)]
            return {
                "before": _clean_state(self._states[self._selected - 1]),
                "after": _clean_state(self._states[self._selected]),
                "move": copy.deepcopy(self._comment_moves[self._selected - 1]),
                "step": self._selected,
                "path": path,
            }
        return {}

    def commentSession(self) -> dict:
        if self._trusted_ancestry and self._states:
            return {
                "schema": "clp.gui-comment-game/1",
                "root": copy.deepcopy(self._states[0]),
                "moves": copy.deepcopy(self._comment_moves),
                "selected_step": self._selected,
            }
        return {}

    def restoreCommentSession(self, session: dict, error: Optional[List[str]] = None) -> bool:
        if not isinstance(session, dict):
            self.last_error = "Invalid saved comment game or unsupported Core root"
            _set_error(error, self.last_error)
            return False
        restored = CoreBridge()
        moves = session.get("moves")
        selected = session.get("selected_step")
        if (
            session.get("schema") != "clp.gui-comment-game/1"
            or not isinstance(moves, list)
            or len(moves) > 10000
            or type(selected) is not int
            or selected < 0
            or selected > len(moves)
            or session.get("root") != restored.commentSession().get("root")
        ):
            self.last_error = "Invalid saved comment game or unsupported Core root"
            _set_error(error, self.last_error)
            return False
        for move in moves:
            if not isinstance(move, dict) or not restored.playEngineMove(move, error):
                self.last_error = restored.last_error
                return False
        if not restored.selectPly(selected, error):
            self.last_error = restored.last_error
            return False
        self._states = restored._states
        self._names = restored._names
        self._comment_moves = restored._comment_moves
        self._selected = restored._selected
        self._trusted_ancestry = restored._trusted_ancestry
        self._legal_cache = restored._legal_cache
        self.last_error = ""
        return True

    def board(self) -> DisplayBoard:
        disp = DisplayBoard()
        cur = self._selected_state()
        if cur is None:
            return disp
        pos = cur["position"]
        disp.valid = True
        disp.camTurn = pos.get("to_move") == "CAM"
        board_arr = pos.get("board", [])
        code_map = {
            "VUONG": "V",
            "TUONG": "T",
            "SI": "S",
            "KI": "K",
            "XA": "X",
            "LONG": "L",
            "PHUONG": "P",
            "BINH": "B",
        }
        for i in range(min(81, len(board_arr))):
            stack = board_arr[i]
            if not stack:
                continue
            top = stack[-1]
            cell = disp.cells[i]
            cell.piece = code_map.get(top.get("type", ""), "?")
            cell.cam = top.get("owner") == "CAM"
            cell.layers = len(stack)
            cell.facing = int(top.get("facing", 0))
        return disp

    def state64(self) -> str:
        cur = self._selected_state()
        if cur is None or not self._trusted_ancestry:
            return ""
        try:
            raw = json.dumps(cur, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
            return base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")
        except Exception:
            return ""

    def canSearch(self) -> bool:
        cur = self._selected_state()
        return bool(cur is not None and self._trusted_ancestry and cur.get("result", {}).get("status") != "terminal")

    def isTerminal(self) -> bool:
        cur = self._selected_state()
        return bool(cur is not None and cur.get("result", {}).get("status") == "terminal")

    def camToMove(self) -> bool:
        cur = self._selected_state()
        return True if cur is None else (cur.get("position", {}).get("to_move") == "CAM")

    def currentPly(self) -> int:
        cur = self._selected_state()
        return int(cur.get("ply", 0)) if cur is not None else 0

    def currentStep(self) -> int:
        return self._selected

    def historyNames(self) -> List[str]:
        return list(self._names)

    def formatPrincipalVariation(self, pv: list) -> str:
        cur = self._selected_state()
        if cur is None or not self._trusted_ancestry:
            return ""
        try:
            state = copy.deepcopy(cur)
            texts: List[str] = []
            for count, item in enumerate(pv, start=1):
                if not isinstance(item, dict):
                    return "[PV verification failed]"
                if count > 32:
                    break
                move = _decode_move_py(item)
                legal_moves, legal_names = self._compute_legal_and_names(state)
                matched_idx = -1
                for idx, cand in enumerate(legal_moves):
                    c_move = _decode_move_py(cand)
                    if (
                        c_move.kind == move.kind
                        and c_move.src == move.src
                        and c_move.dst == move.dst
                        and c_move.ptype == move.ptype
                        and c_move.facing == move.facing
                        and c_move.rotate_after == move.rotate_after
                    ):
                        matched_idx = idx
                        break
                if matched_idx < 0:
                    return "[PV verification failed]"
                texts.append(legal_names[matched_idx])
                state = self._apply_move_to_state(state, legal_moves[matched_idx])
                if state.get("result", {}).get("status") == "terminal":
                    break
            if len(pv) > 32:
                texts.append("...")
            return " ".join(texts)
        except Exception:
            return "[PV verification failed]"

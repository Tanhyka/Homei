"""RecordIO: Safe source-preserving import/export gateway (port of CLP_GUI_TRAINING/src/record_io)."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum
import json
import os
from pathlib import Path
import re
from typing import Any, Dict, List, Optional, Tuple
import uuid


class RecordKind(Enum):
    None_ = 0
    Lpgf11 = 1
    Lpgf20 = 2
    Lppn = 3
    Txt = 4


# Allow ImportedRecord.Kind.* access exactly like C++ enum class
class _KindNamespace:
    None_ = RecordKind.None_
    Lpgf11 = RecordKind.Lpgf11
    Lpgf20 = RecordKind.Lpgf20
    Lppn = RecordKind.Lppn
    Txt = RecordKind.Txt


setattr(_KindNamespace, "None", RecordKind.None_)


@dataclass
class ImportedRecord:
    Kind = _KindNamespace

    kind: RecordKind = RecordKind.None_
    path: str = ""
    originalBytes: bytes = b""
    plainText: str = ""
    notationPreview: List[str] = field(default_factory=list)
    metadata: Dict[str, str] = field(default_factory=dict)

    def kindText(self) -> str:
        if self.kind == RecordKind.Lpgf11:
            return "LPGF 1.1"
        if self.kind == RecordKind.Lpgf20:
            return "LPGF 2.0"
        if self.kind == RecordKind.Lppn:
            return "LPPN lppn1"
        if self.kind == RecordKind.Txt:
            return "TXT Record"
        return "N/A"


def _set_error(error_out: Optional[List[str]], message: str) -> None:
    if isinstance(error_out, list):
        error_out.clear()
        error_out.append(message)


# Historical TXT parsing tables matching ColongphuongGUI_1
_ROWS = ["a", "b", "c", "d", "e", "f", "g", "h", "i"]
_COLS = ["9", "8", "7", "6", "5", "4", "3", "2", "1"]
_DIR_MAP = {"^": 0, ">": 1, "v": 2, "˅": 2, "<": 3, "˄": 0, "V": 2}
_PTYPE_CODE = {
    "VUONG": "V",
    "TUONG": "T",
    "SI": "S",
    "KI": "K",
    "XA": "X",
    "LONG": "L",
    "PHUONG": "P",
    "BINH": "B",
}
_NON_ROTATABLE = {"VUONG", "LONG", "PHUONG"}

_HEADER_KEYS = [
    ("room_name", "Tên Phòng:"),
    ("date", "Ngày Giờ Chơi:"),
    ("winner", "Kỳ Thủ Thắng:"),
    ("player_cam", "Kỳ Thủ Cầm Cam:"),
    ("player_xanh", "Kỳ Thủ Cầm Xanh:"),
]

_TXT_MOVE_BODY_PATTERN = re.compile(
    r"(?:(?:Resign|Xin\s*thua|Xin\s*hàng|Xin\s*hang)|#|"
    r"(?:\[[VTSKXLBP]\]|[VTSKXLBP])\*?(?:[a-i][1-9]|=)"
    r"[>^<˅˄vV]?(?:\.[a-i][1-9])?)",
    re.IGNORECASE,
)

_MOVE_PATTERN = re.compile(
    r"^\s*(\d+)\.\s*(?:(Resign|Xin\s*thua|Xin\s*hàng|Xin\s*hang)|"
    r"(?:(\[?)([VTSKXLBP])\]?(\*?)([a-i][1-9]|=)([>^<˅˄vV]?)(?:\.([a-i][1-9]))?))\s*$",
    re.IGNORECASE,
)


def sq_to_pos(sq_str: str) -> Optional[Tuple[int, int]]:
    """Converts algebraic square string (e.g. 'b4') to (row, col) indices."""
    if len(sq_str) < 2 or sq_str[0].lower() not in _ROWS or sq_str[1] not in _COLS:
        return None
    return _ROWS.index(sq_str[0].lower()), _COLS.index(sq_str[1])


def pos_to_sq(pos: Tuple[int, int]) -> str:
    """Converts (row, col) matrix index to algebraic board notation (e.g. (1, 5) -> 'b4')."""
    return f"{_ROWS[pos[0]]}{_COLS[pos[1]]}"


def tokenize_txt_moves(content: str) -> List[str]:
    """Tokenize compact TXT records without confusing square digits with move numbers."""
    first = re.search(r"(?<!\S)1\.", content)
    if not first:
        first = re.search(r"(?<!\d)1\.", content)
    if not first:
        return []
    pos = first.start()
    move_no = 1
    moves: List[str] = []
    n = len(content)
    while pos < n:
        while pos < n and content[pos].isspace():
            pos += 1
        if pos >= n:
            break
        marker = f"{move_no}."
        if not content.startswith(marker, pos):
            next_pos = content.find(marker, pos)
            if next_pos < 0:
                break
            pos = next_pos
        pos += len(marker)
        while pos < n and content[pos].isspace():
            pos += 1
        body_match = _TXT_MOVE_BODY_PATTERN.match(content, pos)
        if not body_match:
            break
        body = body_match.group(0)
        moves.append(f"{move_no}.{body}")
        pos = body_match.end()
        move_no += 1
    return moves


def _filter_candidates(
    legal_moves: List[dict],
    board: Any,
    is_drop: bool,
    is_team: bool,
    dst_pos: Tuple[int, int],
    src_pos: Optional[Tuple[int, int]],
    pcode: str,
    dir_sym: Optional[str],
    target_facing: int,
) -> List[int]:
    matching = []
    for idx, lm in enumerate(legal_moves):
        lm_kind = lm.get("kind")
        lm_src = tuple(lm["src"]) if lm.get("src") else None
        lm_dst = tuple(lm["dst"]) if lm.get("dst") else None
        eff_dst = lm_src if lm_kind == "rotate" else lm_dst
        if eff_dst != dst_pos:
            continue
        if is_drop and lm_kind != "drop":
            continue
        if not is_drop and (
            (is_team and lm_kind != "team")
            or (not is_team and lm_kind not in ("single", "rotate"))
        ):
            continue
        if src_pos and lm_src != src_pos:
            continue
        if is_drop:
            if _PTYPE_CODE.get(lm.get("ptype")) != pcode.upper():
                continue
        else:
            sq_src = board.sq(*lm_src)
            if sq_src.is_empty or _PTYPE_CODE.get(sq_src.top.ptype.name) != pcode.upper():
                continue
        ptype_name = lm.get("ptype")
        if ptype_name not in _NON_ROTATABLE:
            if lm_kind == "drop":
                if lm.get("facing") != target_facing:
                    continue
            elif not dir_sym:
                if lm.get("rotate_after", 0) % 4 != 0:
                    continue
            else:
                sq_src = board.sq(*lm_src)
                init_f = sq_src.top.facing if not sq_src.is_empty else 0
                if (init_f + lm.get("rotate_after", 0)) % 4 != target_facing:
                    continue
        matching.append(idx)
    return matching


def _resolve_candidates(
    indices: List[int],
    legal_moves: List[dict],
    src_pos: Optional[Tuple[int, int]],
    dst_pos: Tuple[int, int],
) -> Optional[int]:
    unique = []
    seen = set()
    for idx in indices:
        m = legal_moves[idx]
        key = (
            m.get("kind"),
            tuple(m["src"]) if m.get("src") else None,
            tuple(m["dst"]) if m.get("dst") else None,
            m.get("ptype"),
            m.get("facing"),
            m.get("rotate_after", 0) % 4,
        )
        if key in seen:
            continue
        seen.add(key)
        unique.append(idx)
    if not unique:
        return None
    if src_pos is not None:
        exact = [idx for idx in unique if legal_moves[idx].get("src") == list(src_pos)]
        return exact[0] if len(exact) == 1 else None
    rotations = [
        idx
        for idx in unique
        if legal_moves[idx].get("kind") == "rotate" and legal_moves[idx].get("src") == list(dst_pos)
    ]
    if len(rotations) == 1:
        return rotations[0]
    if rotations:
        return None
    sources = {
        tuple(legal_moves[idx]["src"]) if legal_moves[idx].get("src") else None for idx in unique
    }
    if len(sources) == 1 and len(unique) == 1:
        return unique[0]
    return None


class RecordIO:
    @staticmethod
    def read(path: str, output: Optional[ImportedRecord], error: Optional[List[str]] = None) -> bool:
        if output is None:
            _set_error(error, "Null output")
            return False
        p = Path(path)
        if not p.is_file():
            _set_error(error, f"File does not exist: {path}")
            return False
        try:
            size = p.stat().st_size
            if size > 20 * 1024 * 1024:
                _set_error(error, "File too large (max 20 MiB for this GUI)")
                return False
            raw_bytes = p.read_bytes()
        except OSError as ex:
            _set_error(error, str(ex))
            return False

        if not raw_bytes or b"\x00" in raw_bytes:
            _set_error(error, "Empty/binary record rejected")
            return False

        content_bytes = raw_bytes[3:] if raw_bytes.startswith(b"\xef\xbb\xbf") else raw_bytes
        try:
            text = content_bytes.decode("utf-8", errors="strict")
            if text.encode("utf-8") != content_bytes:
                _set_error(error, "Invalid UTF-8")
                return False
        except UnicodeDecodeError:
            _set_error(error, "Invalid UTF-8")
            return False

        rec = ImportedRecord()
        ext = p.suffix.lower().lstrip(".")
        if ext == "lppn":
            if not re.match(r"^\s*lppn1\s+\S+\s+[cx]\s+\S+\s+\S+\s+\d+\s*$", text, flags=re.DOTALL):
                _set_error(error, "Unknown LPPN header/shape; not loaded")
                return False
            rec.kind = RecordKind.Lppn
            rec.notationPreview.append("LPPN: requires strict C++ Core validation before activating board")
        elif ext == "lpgf":
            first = text.strip()
            first_lower = first.lower()
            if first_lower.startswith("#lpgf 2.0"):
                rec.kind = RecordKind.Lpgf20
            elif first_lower.startswith("#lpgf version=1.1") or first_lower.startswith("#lpgf 1.1"):
                rec.kind = RecordKind.Lpgf11
            else:
                _set_error(error, "Only LPGF 1.1 and 2.0 accepted in V0.1")
                return False
            for line in text.split("\n"):
                s = line.strip()
                if not s or s.startswith("#") or s.startswith("{"):
                    continue
                rec.notationPreview.append(s[:200])
                if len(rec.notationPreview) >= 5000:
                    break
            if not rec.notationPreview:
                rec.notationPreview.append("Record imported; no supported notation preview")
        elif (
            ext == "txt"
            or re.search(r"(?im)^\s*Kỳ\s+Phổ\s+Game\s+Đấu\s*$", text)
            or any(len(re.findall(r"(?<!\d)\d+\.", line)) > 1 for line in text.splitlines())
        ):
            rec.kind = RecordKind.Txt
            # Parse historical metadata headers
            for meta_key, prefix in _HEADER_KEYS:
                m_hdr = re.search(r"(?im)^\s*" + re.escape(prefix) + r"\s*(.*)$", text)
                if m_hdr:
                    val = m_hdr.group(1).strip()
                    if val:
                        rec.metadata[meta_key] = val

            tokens = tokenize_txt_moves(text)
            rec.notationPreview = tokens[:5000]
            if not rec.notationPreview:
                rec.notationPreview.append("Record imported; no numbered moves found")
        else:
            _set_error(error, "Expected .txt, .lpgf, or .lppn")
            return False

        output.kind = rec.kind
        output.path = str(path)
        output.originalBytes = raw_bytes
        output.plainText = text
        output.notationPreview = rec.notationPreview
        output.metadata = rec.metadata
        return True

    @staticmethod
    def load_txt_moves_into_bridge(
        core_bridge: Any,
        record_or_text: ImportedRecord | str,
        progress_callback: Optional[Any] = None,
    ) -> Tuple[int, int, Optional[str]]:
        """Parses TXT/LPGF numbered moves and applies them with maximum performance.

        Uses fast in-memory candidate matching (pseudo-legal generation + king safety check
        only on matching candidates) without expensive IPC/formatting roundtrips.

        Returns:
            Tuple[applied_count, total_count, error_msg_if_any]
        """
        from .board import Board
        from .pieces import Player
        from . import rules
        from .core_bridge import (
            _encode_position_py,
            _encode_move_py,
            _position_identity_py,
        )

        content = record_or_text.plainText if isinstance(record_or_text, ImportedRecord) else str(record_or_text)
        tokens = tokenize_txt_moves(content)
        if not tokens:
            for line in content.splitlines():
                s = line.strip()
                if re.match(r"^\d+\.", s):
                    tokens.append(s)

        if not tokens:
            return 0, 0, "No numbered moves found in record"

        board = Board()
        board.setup_initial()
        turn = Player.CAM
        last_dst_pos = None

        pos_dict = _encode_position_py(board, turn)
        initial_state = {
            "notation": None,
            "ply": 0,
            "position": pos_dict,
            "repetition": {
                "complete": True,
                "entries": [{"count": 1, "position": pos_dict}],
            },
            "result": {"status": "ongoing"},
            "schema": "clp.state/1",
        }

        states = [initial_state]
        names: List[str] = []
        moves_history: List[dict] = []
        applied = 0
        err_msg = None
        ending = None

        for token in tokens:
            line = token.strip()
            if not line:
                continue

            # Checkmate indicator '#'
            if re.match(r"^\s*(\d+)\.\s*#\s*$", line):
                ending = "mate"
                break

            m = _MOVE_PATTERN.match(line)
            if not m:
                err_msg = f"Syntax error at {line}"
                break

            move_no_str, resign_tag, is_team_open, pcode, is_drop_str, dst_sq, dir_sym, src_sq = m.groups()
            move_no = int(move_no_str)
            if resign_tag:
                ending = "resign"
                break

            is_drop = bool(is_drop_str)
            dst_pos = last_dst_pos if dst_sq == "=" else sq_to_pos(dst_sq)
            if dst_pos is None:
                err_msg = f"Invalid destination square '{dst_sq}' at move {move_no}"
                break
            last_dst_pos = dst_pos
            is_team = bool(is_team_open)
            src_pos = sq_to_pos(src_sq) if src_sq else None
            target_facing = _DIR_MAP.get(dir_sym) if dir_sym else (2 if turn == Player.CAM else 0)

            # Fast pseudo-legal generation (no full king-safety overhead)
            candidates = rules.generate_moves(board, turn)

            def match_candidate(lm, check_team: bool) -> bool:
                if is_drop and lm.kind != "drop":
                    return False
                if not is_drop and (
                    (check_team and lm.kind != "team")
                    or (not check_team and lm.kind not in ("single", "rotate"))
                ):
                    return False
                eff_dst = lm.src if lm.kind == "rotate" else lm.dst
                if eff_dst != dst_pos:
                    return False
                if src_pos and lm.src != src_pos:
                    return False
                if is_drop:
                    if _PTYPE_CODE.get(lm.ptype.name if lm.ptype else "") != pcode.upper():
                        return False
                else:
                    sq_src = board.sq(*lm.src)
                    if sq_src.is_empty or _PTYPE_CODE.get(sq_src.top.ptype.name) != pcode.upper():
                        return False
                if lm.ptype and lm.ptype.name not in _NON_ROTATABLE:
                    if lm.kind == "drop":
                        if lm.facing != target_facing:
                            return False
                    elif not dir_sym:
                        if lm.rotate_after % 4 != 0:
                            return False
                    else:
                        sq_src = board.sq(*lm.src)
                        init_f = sq_src.top.facing if not sq_src.is_empty else 0
                        if (init_f + lm.rotate_after) % 4 != target_facing:
                            return False
                return True

            matching = [lm for lm in candidates if match_candidate(lm, is_team)]
            if not matching and is_team:
                matching = [lm for lm in candidates if match_candidate(lm, False)]
            elif not matching and not is_team:
                matching = [lm for lm in candidates if match_candidate(lm, True)]

            # Check king safety ONLY for the notation-filtered candidate subset
            legal_matching = []
            for lm in matching:
                try:
                    b2 = board.clone()
                    rules.apply_move(b2, turn, lm)
                    if not rules.is_in_check(b2, turn):
                        legal_matching.append(lm)
                except Exception:
                    pass

            if not legal_matching:
                err_msg = f"Cannot resolve move {move_no}: {line} (matching={len(matching)})"
                break

            # Resolve disambiguation
            choice = None
            if src_pos is not None:
                exact = [mv for mv in legal_matching if mv.src == src_pos]
                choice = exact[0] if len(exact) == 1 else None
            else:
                rotations = [mv for mv in legal_matching if mv.kind == "rotate" and mv.src == dst_pos]
                if len(rotations) == 1:
                    choice = rotations[0]
                elif not rotations:
                    sources = {mv.src for mv in legal_matching}
                    if len(sources) == 1 and len(legal_matching) == 1:
                        choice = legal_matching[0]

            if choice is None:
                choice = legal_matching[0]

            # Apply move to board
            rules.apply_move(board, turn, choice)
            turn = rules.other(turn)
            applied += 1

            move_notation = line.split(".", 1)[1].strip() if "." in line else line
            move_dict = _encode_move_py(choice, move_notation)
            next_pos = _encode_position_py(board, turn)
            next_id = _position_identity_py(next_pos)

            prev_reps = states[-1]["repetition"]["entries"]
            new_reps = [dict(e) for e in prev_reps]
            found = False
            for entry in new_reps:
                if _position_identity_py(entry["position"]) == next_id:
                    entry["count"] += 1
                    found = True
                    break
            if not found:
                new_reps.append({"count": 1, "position": next_pos})

            next_state = {
                "notation": {
                    "format": "TXT",
                    "text": move_notation,
                    "legacy_turnabout": False,
                },
                "ply": applied,
                "position": next_pos,
                "repetition": {"complete": True, "entries": new_reps},
                "result": {"status": "ongoing"},
                "schema": "clp.state/1",
            }
            states.append(next_state)
            names.append(move_notation)
            moves_history.append(move_dict)

            if progress_callback is not None and applied % 10 == 0:
                progress_callback(applied, len(tokens))

        if applied > 0:
            core_bridge.loadHistoryStates(states, names, moves_history, applied, ending)
        else:
            core_bridge.reset()

        return applied, len(tokens), err_msg

    @staticmethod
    def exportUnmodified(rec: ImportedRecord, output_path: str, error: Optional[List[str]] = None) -> bool:
        if rec.kind == RecordKind.None_ or not rec.originalBytes:
            _set_error(error, "No imported source to export")
            return False
        dest = Path(output_path)
        if dest.exists():
            _set_error(error, "Refusing to overwrite an existing file")
            return False
        if rec.path and Path(rec.path).resolve() == dest.resolve():
            _set_error(error, "Refusing to overwrite original record")
            return False
        try:
            fd = os.open(str(dest), os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o666)
        except OSError as ex:
            _set_error(error, str(ex))
            return False
        try:
            with os.fdopen(fd, "wb") as f:
                written = f.write(rec.originalBytes)
                f.flush()
                if written != len(rec.originalBytes):
                    raise OSError("Incomplete write")
        except OSError as ex:
            _set_error(error, str(ex))
            try:
                dest.unlink(missing_ok=True)
            except OSError:
                pass
            return False
        return True

    @staticmethod
    def tokenize_txt_moves(content: str) -> List[str]:
        return tokenize_txt_moves(content)

    @staticmethod
    def exportRecord(
        core_bridge: Any,
        output_path: str,
        format_type: str = "txt",
        metadata: Optional[Dict[str, Any]] = None,
        error: Optional[List[str]] = None,
    ) -> bool:
        dest = Path(output_path)
        try:
            fmt = format_type.lower().strip().lstrip(".")
            if fmt == "lpgf" or dest.suffix.lower() == ".lpgf":
                content = export_to_lpgf_string(core_bridge, metadata)
            else:
                content = export_to_txt_string(core_bridge, metadata)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(content, encoding="utf-8")
            return True
        except Exception as ex:
            _set_error(error, str(ex))
            return False


def export_to_txt_string(core_bridge: Any, metadata: Optional[Dict[str, Any]] = None) -> str:
    """Exports the active mainline game history to the standard 6-column Vietnamese tournament TXT format."""
    meta = dict(metadata or {})
    lines = [
        "Kỳ Phổ Game Đấu",
        f"Tên Phòng: {meta.get('room_name') or meta.get('room') or 'Phòng tập luyện'}",
        f"Ngày Giờ Chơi: {meta.get('play_time') or meta.get('date') or datetime.now().strftime('%d-%m-%Y %H:%M')}",
        f"Kỳ Thủ Thắng: {meta.get('winner') or 'Chưa kết thúc'}",
        f"Kỳ Thủ Cầm Cam: {meta.get('player_cam') or 'Cam'}",
        f"Kỳ Thủ Cầm Xanh: {meta.get('player_xanh') or 'Xanh'}",
        "",
    ]
    active_func = getattr(core_bridge, "_active_line_nodes", None)
    if callable(active_func):
        nodes = active_func()
        moves = [n.notation_text for n in nodes[1:] if getattr(n, "notation_text", None)]
    else:
        moves = list(core_bridge.historyNames())

    cols = 6
    for i in range(0, len(moves), cols):
        chunk = moves[i : i + cols]
        row_parts = []
        for j, mv in enumerate(chunk):
            move_num = i + j + 1
            entry = f"{move_num}.{mv}"
            row_parts.append(f"{entry:<18}")
        lines.append("".join(row_parts).rstrip())
    lines.append("")
    return "\n".join(lines)


def export_to_lpgf_string(core_bridge: Any, metadata: Optional[Dict[str, Any]] = None) -> str:
    """Exports the entire MoveTree structure (including branches and variations) to LPGF 2.0 format."""
    from .pieces import Player
    from .core_bridge import _decode_position_py

    root_node = getattr(core_bridge, "root_node", None)
    if root_node is None or not root_node.state:
        from .board import Board
        root_board = Board()
        root_board.setup_initial()
        root_turn = Player.CAM
        root_ply = 0
    else:
        root_board, root_turn = _decode_position_py(root_node.state["position"])
        root_ply = root_node.ply

    # Build squares payload for root
    squares = []
    for r in range(9):
        for c in range(9):
            stack = root_board.sq(r, c).stack
            if not stack:
                continue
            squares.append([
                r,
                c,
                [[p.ptype.name, p.owner.name, int(p.facing) % 4] for p in stack],
            ])

    root_payload = {
        "current": root_turn.name,
        "squares": squares,
        "reserve": {
            pl.name: [ptype.name for ptype in root_board.reserve[pl]]
            for pl in Player
        },
        "prisoners": {
            pl.name: [ptype.name for ptype in root_board.prisoners[pl]]
            for pl in Player
        },
    }

    entries = []
    nodes = root_node.all_nodes() if root_node is not None else []
    for n in nodes:
        m_data = None
        if n.move is not None:
            ptype_val = n.move.get("ptype")
            if hasattr(ptype_val, "name"):
                ptype_name = ptype_val.name
            elif isinstance(ptype_val, str):
                ptype_name = ptype_val
            else:
                ptype_name = None
            m_data = {
                "kind": n.move.get("kind"),
                "src": list(n.move["src"]) if n.move.get("src") else None,
                "dst": list(n.move["dst"]) if n.move.get("dst") else None,
                "ptype": ptype_name,
                "facing": n.move.get("facing"),
                "rotate_after": (n.move.get("rotate_after", 0) % 4) if n.move.get("rotate_after") is not None else 0,
            }

        entries.append({
            "id": n.node_id,
            "parent": n.parent.node_id if n.parent else None,
            "move": m_data,
            "notation": n.notation_text or "",
            "comment": n.comment or "",
            "glyphs": list(n.glyphs or []),
            "metadata": dict(n.metadata or {}),
            "result": n.result,
        })

    meta = dict(metadata or {})
    current_node = getattr(core_bridge, "current_node", None)
    selected_id = current_node.node_id if current_node else (root_node.node_id if root_node else "")
    payload = {
        "schema": "lpgf.tree/2.0-provisional",
        "game_id": meta.get("game_id") or uuid.uuid4().hex,
        "root": root_payload,
        "base_ply": root_ply,
        "max_plies": None,
        "selected": selected_id,
        "nodes": entries,
        "metadata": meta,
        "extensions": {},
    }
    return "#lpgf 2.0\n" + json.dumps(payload, ensure_ascii=False, indent=2) + "\n"

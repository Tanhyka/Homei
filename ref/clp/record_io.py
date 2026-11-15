"""RecordIO: Safe source-preserving import/export gateway (LPGF 2.0 & TXT standards)."""

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
    branches: List[Dict[str, Any]] = field(default_factory=list)
    custom_root: Optional[Tuple[Any, Any, int]] = None

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
_DIR_SYMBOLS = {0: "^", 1: ">", 2: "v", 3: "<"}
_FULLWIDTH_COLS = "  ９ ８ ７ ６ ５ ４ ３ ２ １"

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

_CODE_TO_PTYPE_NAME = {v: k for k, v in _PTYPE_CODE.items()}
_NON_ROTATABLE = {"VUONG", "LONG", "PHUONG"}

_HEADER_KEYS = [
    ("room_name", "Tên Phòng:"),
    ("date", "Ngày Giờ Chơi:"),
    ("save_date", "Ngày Giờ Lưu:"),
    ("winner", "Kỳ Thủ Thắng:"),
    ("player_cam", "Kỳ Thủ Cầm Cam:"),
    ("player_xanh", "Kỳ Thủ Cầm Xanh:"),
]

# Pattern matching moves in TXT and LPGF:
# Supports 1.P*b7, 5.Sb8(a8), 9.Bd8.d9(c8), 17.Bc2<(c3), 19.[B]c6>(c6), 40.L=(c5)
# Also supports branches formatted as '50 L*b5', '51 Sb8(a8)', '100. #'
_TXT_MOVE_BODY_PATTERN = re.compile(
    r"(?:(?:Resign|Xin\s*thua|Xin\s*hàng|Xin\s*hang)|#|"
    r"(?:\[[VTSKXLBP]\]|[VTSKXLBP])\*?(?:[a-i][1-9]|=)"
    r"[>^<˅˄vV]?(?:\.[a-i][1-9])?(?:\([a-i][1-9]\))?)",
    re.IGNORECASE,
)

_MOVE_PATTERN = re.compile(
    r"^\s*(\d+)(?:[\.\s]\s*|\s+)(?:(Resign|Xin\s*thua|Xin\s*hàng|Xin\s*hang)|"
    r"(?:(\[?)([VTSKXLBP])\]?(\*?)([a-i][1-9]|=)([>^<˅˄vV]?)(?:\.([a-i][1-9]))?(?:\(([a-i][1-9])\))?))\s*$",
    re.IGNORECASE,
)

_BRANCH_HEADER_PATTERN = re.compile(
    r"^\s*(?:nhánh|nhanh)\s*:\s*(?:nước|nuoc)\s*(?:thứ|thu)?\s*(\d+)",
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


def strip_source_square(notation: str) -> str:
    """Strips parenthesized source square (e.g. 'Sb8(a8)' -> 'Sb8', '19.[B]c6>(c6)' -> '19.[B]c6>')."""
    return re.sub(r"\([a-i][1-9]\)$", "", notation.strip())


def detect_ascii_board(text: str) -> bool:
    """Detects whether text contains a 9x9 ASCII CLP board diagram."""
    return bool(re.search(r"\+[-]{27}\+", text) and re.search(r"\|\s*.*?\s*\|[a-i]", text, re.IGNORECASE))


def format_ascii_board(board: Any, turn: Any, base_ply: int = 0) -> str:
    """Formats a CLP Board, turn, and starting ply into standard 9x9 ASCII board diagram."""
    from .pieces import Piece, PieceType, Player

    def format_piece(p: Piece) -> str:
        code = _PTYPE_CODE[p.ptype.name]
        prefix = "c" if p.owner == Player.CAM else ""
        default_f = 2 if p.owner == Player.CAM else 0
        can_rot = p.ptype.name not in _NON_ROTATABLE
        facing_sym = ""
        if can_rot and (p.facing % 4) != default_f:
            facing_sym = _DIR_SYMBOLS[p.facing % 4]
        return f"{prefix}{code}{facing_sym}"

    def format_cell(sq) -> str:
        if sq.is_empty:
            return "∗"
        if len(sq.stack) == 1:
            return format_piece(sq.top)
        inner = "".join(format_piece(p) for p in sq.stack)
        return f"[{inner}]"

    def format_hand(hand_pieces: list) -> str:
        if not hand_pieces:
            return "không"
        counts: Dict[Any, int] = {}
        for pt in hand_pieces:
            counts[pt] = counts.get(pt, 0) + 1
        order = [
            PieceType.BINH,
            PieceType.KI,
            PieceType.XA,
            PieceType.SI,
            PieceType.TUONG,
            PieceType.LONG,
            PieceType.PHUONG,
            PieceType.VUONG,
        ]
        parts = []
        for pt in sorted(counts.keys(), key=lambda x: order.index(x) if x in order else 99):
            c = counts[pt]
            code = _PTYPE_CODE[pt.name]
            parts.append(f"{code}{c}" if c > 1 else f"{code}")
        return " ".join(parts)

    lines = []
    cam_hand = board.prisoners[Player.CAM] + board.reserve[Player.CAM]
    lines.append(f"Cam: {format_hand(cam_hand)}")
    lines.append(_FULLWIDTH_COLS)
    lines.append("+---------------------------+")
    for r in range(9):
        cells = [format_cell(board.sq(r, c)) for c in range(9)]
        row_str = " ".join(cells)
        lines.append(f"| {row_str}|{_ROWS[r]}")
    lines.append("+---------------------------+")
    xanh_hand = board.prisoners[Player.XANH] + board.reserve[Player.XANH]
    lines.append(f"Xanh: {format_hand(xanh_hand)}")
    turn_str = "Cam" if turn == Player.CAM else "Xanh"
    lines.append(f"Lượt đi: {turn_str}")
    if base_ply > 0:
        lines.append(f"{base_ply} ...")
    return "\n".join(lines)


def parse_ascii_board(text: str) -> Optional[Tuple[Any, Any, int]]:
    """Parses a 9x9 ASCII CLP board diagram into (Board, Player, base_ply)."""
    if not detect_ascii_board(text):
        return None
    from .board import Board
    from .pieces import Piece, PieceType, Player

    code_to_ptype = {
        "V": PieceType.VUONG,
        "T": PieceType.TUONG,
        "S": PieceType.SI,
        "K": PieceType.KI,
        "X": PieceType.XA,
        "L": PieceType.LONG,
        "P": PieceType.PHUONG,
        "B": PieceType.BINH,
    }

    board = Board()
    board.reserve = {Player.CAM: [], Player.XANH: []}
    board.prisoners = {Player.CAM: [], Player.XANH: []}
    turn = Player.CAM
    base_ply = 0

    lines = [l.strip() for l in text.strip().splitlines() if l.strip()]

    # 1. Cam hand
    m_cam = re.search(r"Cam\s*:\s*(.*)", text, re.IGNORECASE)
    if m_cam:
        raw = m_cam.group(1).strip()
        if raw.lower() != "không":
            for token in raw.split():
                m_t = re.match(r"([VLPTSKXB])(\d*)", token, re.IGNORECASE)
                if m_t:
                    code, cnt_s = m_t.groups()
                    cnt = int(cnt_s) if cnt_s else 1
                    pt = code_to_ptype[code.upper()]
                    board.prisoners[Player.CAM].extend([pt] * cnt)

    # 2. Rows
    row_pattern = re.compile(r"^\|\s*(.*?)\s*\|([a-i])\s*$", re.IGNORECASE)
    piece_pattern = re.compile(r"(c?)([VLPTSKXB])([>^v<]?)", re.IGNORECASE)
    row_map = {ch: i for i, ch in enumerate(_ROWS)}

    for line in lines:
        m_r = row_pattern.match(line)
        if m_r:
            content_str, r_ch = m_r.groups()
            r_idx = row_map[r_ch.lower()]
            tokens = content_str.split()
            if len(tokens) != 9:
                continue
            for c_idx, tok in enumerate(tokens):
                if tok in ("∗", "*", "."):
                    continue
                if tok.startswith("[") and tok.endswith("]"):
                    inner = tok[1:-1]
                    matches = piece_pattern.findall(inner)
                    for c_prefix, pcode, f_sym in matches:
                        owner = Player.CAM if c_prefix.lower() == "c" else Player.XANH
                        pt = code_to_ptype[pcode.upper()]
                        default_f = 2 if owner == Player.CAM else 0
                        facing = _DIR_MAP[f_sym] if f_sym else default_f
                        board.sq(r_idx, c_idx).stack.append(Piece(pt, owner, facing))
                else:
                    m_p = re.match(r"^(c?)([VLPTSKXB])([>^v<]?)$", tok, re.IGNORECASE)
                    if m_p:
                        c_prefix, pcode, f_sym = m_p.groups()
                        owner = Player.CAM if c_prefix.lower() == "c" else Player.XANH
                        pt = code_to_ptype[pcode.upper()]
                        default_f = 2 if owner == Player.CAM else 0
                        facing = _DIR_MAP[f_sym] if f_sym else default_f
                        board.sq(r_idx, c_idx).stack.append(Piece(pt, owner, facing))

    # 3. Xanh hand
    m_xanh = re.search(r"Xanh\s*:\s*(.*)", text, re.IGNORECASE)
    if m_xanh:
        raw = m_xanh.group(1).strip()
        if raw.lower() != "không":
            for token in raw.split():
                m_t = re.match(r"([VLPTSKXB])(\d*)", token, re.IGNORECASE)
                if m_t:
                    code, cnt_s = m_t.groups()
                    cnt = int(cnt_s) if cnt_s else 1
                    pt = code_to_ptype[code.upper()]
                    board.prisoners[Player.XANH].extend([pt] * cnt)

    # 4. Turn
    m_turn = re.search(r"Lượt\s*đi\s*:\s*(Cam|Xanh)", text, re.IGNORECASE)
    if m_turn:
        turn = Player.CAM if m_turn.group(1).lower() == "cam" else Player.XANH

    # 5. Ply
    m_ply = re.search(r"(\d+)\s*\.\.\.", text)
    if m_ply:
        base_ply = int(m_ply.group(1))

    return board, turn, base_ply


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


def _extract_branch_blocks(content: str) -> Tuple[str, List[Tuple[int, List[str]]]]:
    """Separates mainline text from branch blocks labeled 'nhánh: nước thứ N'."""
    lines = content.splitlines()
    mainline_lines: List[str] = []
    branches: List[Tuple[int, List[str]]] = []

    curr_branch_ply: Optional[int] = None
    curr_branch_lines: List[str] = []

    for line in lines:
        m_br = _BRANCH_HEADER_PATTERN.match(line)
        if m_br:
            if curr_branch_ply is not None:
                branches.append((curr_branch_ply, curr_branch_lines))
            curr_branch_ply = int(m_br.group(1))
            curr_branch_lines = []
            continue

        if curr_branch_ply is not None:
            curr_branch_lines.append(line)
        else:
            mainline_lines.append(line)

    if curr_branch_ply is not None:
        branches.append((curr_branch_ply, curr_branch_lines))

    return "\n".join(mainline_lines), branches


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

        # Check ASCII board diagram
        if detect_ascii_board(text):
            rec.custom_root = parse_ascii_board(text)

        # Parse standard metadata headers across all file formats
        for meta_key, prefix in _HEADER_KEYS:
            m_hdr = re.search(r"(?im)^\s*" + re.escape(prefix) + r"\s*(.*)$", text)
            if m_hdr:
                val = m_hdr.group(1).strip()
                if val:
                    rec.metadata[meta_key] = val

        # Separate mainline from branch variations
        mainline_text, branch_data = _extract_branch_blocks(text)
        rec.branches = [{"ply": ply, "lines": lines} for ply, lines in branch_data]

        if ext == "lppn":
            if not re.match(r"^\s*lppn1\s+\S+\s+[cx]\s+\S+\s+\S+\s+\d+\s*$", text, flags=re.DOTALL):
                _set_error(error, "Unknown LPPN header/shape; not loaded")
                return False
            rec.kind = RecordKind.Lppn
            rec.notationPreview.append("LPPN: requires strict C++ Core validation before activating board")
        elif ext == "lpgf":
            first = text.strip()
            first_lower = first.lower()
            if first_lower.startswith("#lpgf version=2.0") or first_lower.startswith("#lpgf 2.0"):
                rec.kind = RecordKind.Lpgf20
            elif first_lower.startswith("#lpgf version=1.1") or first_lower.startswith("#lpgf 1.1"):
                rec.kind = RecordKind.Lpgf11
            else:
                _set_error(error, "Only LPGF 1.1 and 2.0 accepted in V0.1")
                return False

            tokens = tokenize_txt_moves(mainline_text)
            if not tokens:
                for line in mainline_text.splitlines():
                    s = line.strip()
                    if re.match(r"^\d+\.", s):
                        tokens.append(s)
            rec.notationPreview = tokens[:5000]
            if not rec.notationPreview:
                rec.notationPreview.append("Record imported; no supported notation preview")
        elif (
            ext == "txt"
            or re.search(r"(?im)^\s*Kỳ\s+Phổ\s+Game\s+Đấu\s*$", text)
            or any(len(re.findall(r"(?<!\d)\d+\.", line)) > 1 for line in text.splitlines())
        ):
            rec.kind = RecordKind.Txt
            tokens = tokenize_txt_moves(mainline_text)
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
        output.branches = rec.branches
        output.custom_root = rec.custom_root
        return True

    @staticmethod
    def load_txt_moves_into_bridge(
        core_bridge: Any,
        record_or_text: ImportedRecord | str,
        progress_callback: Optional[Any] = None,
    ) -> Tuple[int, int, Optional[str]]:
        """Parses TXT/LPGF 2.0 numbered moves and variations, applying them with maximum performance.

        Supports parenthesized source square e.g. '5.Sb8(a8)', '9.Bd8.d9(c8)',
        ASCII 9x9 starting diagrams, and variation blocks 'nhánh: nước thứ N'.
        """
        from .board import Board
        from .pieces import Player
        from . import rules
        from .core_bridge import (
            MoveNode,
            _encode_position_py,
            _encode_move_py,
            _position_identity_py,
            _decode_position_py,
        )

        if isinstance(record_or_text, ImportedRecord):
            content = record_or_text.plainText
            custom_root = record_or_text.custom_root or parse_ascii_board(content)
            branches = record_or_text.branches
            if not branches:
                _, branch_data = _extract_branch_blocks(content)
                branches = [{"ply": ply, "lines": lines} for ply, lines in branch_data]
        else:
            content = str(record_or_text)
            custom_root = parse_ascii_board(content)
            _, branch_data = _extract_branch_blocks(content)
            branches = [{"ply": ply, "lines": lines} for ply, lines in branch_data]

        mainline_text, _ = _extract_branch_blocks(content)
        tokens = tokenize_txt_moves(mainline_text)
        if not tokens:
            for line in mainline_text.splitlines():
                s = line.strip()
                if re.match(r"^\d+\.", s):
                    tokens.append(s)

        if not tokens and not custom_root:
            return 0, 0, "No numbered moves found in record"

        # Initialize root board state
        if custom_root:
            board, turn, base_ply = custom_root
            board = board.clone()
        else:
            board = Board()
            board.setup_initial()
            turn = Player.CAM
            base_ply = 0

        pos_dict = _encode_position_py(board, turn)
        initial_state = {
            "notation": None,
            "ply": base_ply,
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
        last_dst_pos = None

        def apply_token_sequence(
            curr_board: Any,
            curr_turn: Any,
            move_tokens: List[str],
            parent_node: Optional[MoveNode] = None,
        ) -> Tuple[int, Optional[str], Optional[str]]:
            nonlocal last_dst_pos
            seq_applied = 0
            seq_ending = None
            sim_board = curr_board.clone()
            sim_turn = curr_turn
            active_parent = parent_node

            for token in move_tokens:
                line = token.strip()
                if not line:
                    continue

                if re.match(r"^\s*(\d+)\.\s*#\s*$", line):
                    seq_ending = "mate"
                    if active_parent:
                        active_parent.result = f"{rules.other(sim_turn).name} thắng (Chiếu hết)"
                    break

                m = _MOVE_PATTERN.match(line)
                if not m:
                    return seq_applied, f"Cú pháp nước đi không hợp lệ tại {line}", None

                (
                    move_no_str,
                    resign_tag,
                    is_team_open,
                    pcode,
                    is_drop_str,
                    dst_sq,
                    dir_sym,
                    dot_src,
                    paren_src,
                ) = m.groups()

                move_no = int(move_no_str)
                if resign_tag:
                    seq_ending = "resign"
                    if active_parent:
                        active_parent.result = f"{sim_turn.name} xin thua"
                    break

                is_drop = bool(is_drop_str)
                dst_pos = last_dst_pos if dst_sq == "=" else sq_to_pos(dst_sq)
                if dst_pos is None:
                    return seq_applied, f"Invalid destination square '{dst_sq}' at move {move_no}", None
                last_dst_pos = dst_pos
                is_team = bool(is_team_open)
                src_pos = sq_to_pos(paren_src) if paren_src else (sq_to_pos(dot_src) if dot_src else None)
                target_facing = _DIR_MAP.get(dir_sym) if dir_sym else (2 if sim_turn == Player.CAM else 0)

                candidates = rules.generate_moves(sim_board, sim_turn)

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
                        sq_src = sim_board.sq(*lm.src)
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
                            sq_src = sim_board.sq(*lm.src)
                            init_f = sq_src.top.facing if not sq_src.is_empty else 0
                            if (init_f + lm.rotate_after) % 4 != target_facing:
                                return False
                    return True

                matching = [lm for lm in candidates if match_candidate(lm, is_team)]
                if not matching and is_team:
                    matching = [lm for lm in candidates if match_candidate(lm, False)]
                elif not matching and not is_team:
                    matching = [lm for lm in candidates if match_candidate(lm, True)]

                legal_matching = []
                for lm in matching:
                    try:
                        b2 = sim_board.clone()
                        rules.apply_move(b2, sim_turn, lm)
                        if not rules.is_in_check(b2, sim_turn):
                            legal_matching.append(lm)
                    except Exception:
                        pass

                if not legal_matching:
                    if matching:
                        return seq_applied, f"Nước đi không rõ ràng tại nước {move_no}: {line}", None
                    return seq_applied, f"Illegal move tại nước {move_no}: {line}", None

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

                # Apply move
                rules.apply_move(sim_board, sim_turn, choice)
                sim_turn = rules.other(sim_turn)
                seq_applied += 1

                raw_notation = line.split(".", 1)[1].strip() if "." in line else line
                # Strip parenthesized source square for clean GUI display notation
                clean_notation = strip_source_square(raw_notation)
                move_dict = _encode_move_py(choice, clean_notation)

                if active_parent is not None:
                    # Variation branch node
                    next_pos_dict = _encode_position_py(sim_board, sim_turn)
                    child_state = {
                        "notation": {"format": "TXT", "text": clean_notation, "legacy_turnabout": False},
                        "ply": active_parent.ply + 1,
                        "position": next_pos_dict,
                        "repetition": {"complete": True, "entries": []},
                        "result": {"status": "ongoing"},
                        "schema": "clp.state/1",
                    }
                    new_child = MoveNode(
                        move=move_dict,
                        parent=active_parent,
                        notation_text=clean_notation,
                        side=rules.other(sim_turn).name,
                        ply=active_parent.ply + 1,
                        state=child_state,
                    )
                    active_parent.children.append(new_child)
                    active_parent = new_child
                else:
                    # Mainline node
                    next_pos = _encode_position_py(sim_board, sim_turn)
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
                            "text": clean_notation,
                            "legacy_turnabout": False,
                        },
                        "ply": base_ply + seq_applied,
                        "position": next_pos,
                        "repetition": {"complete": True, "entries": new_reps},
                        "result": {"status": "ongoing"},
                        "schema": "clp.state/1",
                    }
                    states.append(next_state)
                    names.append(clean_notation)
                    moves_history.append(move_dict)

                if progress_callback is not None and (applied + seq_applied) % 10 == 0:
                    progress_callback(applied + seq_applied, len(tokens))

            return seq_applied, None, seq_ending

        # 1. Apply mainline moves
        applied, err_msg, ending = apply_token_sequence(board, turn, tokens, parent_node=None)

        if applied > 0 or custom_root:
            core_bridge.loadHistoryStates(states, names, moves_history, applied, ending)
        else:
            core_bridge.reset()

        # 2. Apply variations (Biến nhánh)
        if branches and core_bridge.root_node is not None:
            all_main_nodes = core_bridge.root_node.all_nodes()
            for br in branches:
                br_ply = br["ply"]
                br_lines = br["lines"]
                # Parent of move br_ply is the node at ply (br_ply - 1)
                parent_candidates = [n for n in all_main_nodes if n.ply == br_ply - 1]
                if not parent_candidates:
                    continue
                branch_parent = parent_candidates[0]
                branch_board, branch_turn = _decode_position_py(branch_parent.state["position"])

                # Tokenize branch moves
                br_text = "\n".join(br_lines)
                br_tokens = re.findall(
                    r"\d+(?:[\.\s]\s*|\s+)(?:Resign|Xin\s*thua|Xin\s*hàng|Xin\s*hang|#|"
                    r"(?:\[[VTSKXLBP]\]|[VTSKXLBP])\*?(?:[a-i][1-9]|=)[>^<˅˄vV]?(?:\.[a-i][1-9])?(?:\([a-i][1-9]\))?)",
                    br_text,
                    re.IGNORECASE,
                )
                if br_tokens:
                    apply_token_sequence(branch_board, branch_turn, br_tokens, parent_node=branch_parent)

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
                content = export_to_lpgf_v2_string(core_bridge, metadata)
            else:
                content = export_to_txt_string(core_bridge, metadata)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(content, encoding="utf-8")
            return True
        except Exception as ex:
            _set_error(error, str(ex))
            return False


def _format_move_token(node: Any, sim_board: Any, include_source: bool) -> str:
    """Format one node move into natural notation with or without parenthesized (source)."""
    mv = getattr(node, "move", None)
    raw = getattr(node, "notation_text", "") or ""
    clean = strip_source_square(raw)

    if not mv:
        return clean

    kind = mv.get("kind") if isinstance(mv, dict) else getattr(mv, "kind", None)
    src = tuple(mv["src"]) if (isinstance(mv, dict) and mv.get("src")) else getattr(mv, "src", None)
    rot = (mv.get("rotate_after", 0) % 4) if isinstance(mv, dict) else (getattr(mv, "rotate_after", 0) % 4)

    # In natural text convention, if rotate_after == 0 and kind != rotate, do not include default facing arrows
    if kind != "drop" and rot == 0 and kind != "rotate":
        clean = re.sub(r"([a-i][1-9]|=)[>^<˅˄vV](\.[a-i][1-9])?", r"\1\2", clean)

    if include_source and kind != "drop" and src:
        return f"{clean}({pos_to_sq(src)})"
    return clean


def export_to_lpgf_v2_string(core_bridge: Any, metadata: Optional[Dict[str, Any]] = None) -> str:
    """Exports the entire MoveTree structure to human-readable LPGF 2.0 format with (ô_nguồn) and ASCII diagram."""
    from .board import Board
    from .pieces import Player
    from .core_bridge import _decode_position_py

    meta = dict(metadata or {})
    save_time = meta.get("save_date") or datetime.now().strftime("%d-%m-%Y %H:%M")
    play_time = meta.get("play_time") or meta.get("date") or datetime.now().strftime("%d-%m-%Y %H:%M")
    room_name = meta.get("room_name") or meta.get("room") or "Phòng tập luyện"
    winner = meta.get("winner") or "Chưa kết thúc"
    player_cam = meta.get("player_cam") or "Cam"
    player_xanh = meta.get("player_xanh") or "Xanh"

    lines = [
        "#lpgf version=2.0 encoding=UTF-8",
        f"Tên Phòng: {room_name}",
        f"Ngày Giờ Chơi: {play_time}",
        f"Ngày Giờ Lưu: {save_time}",
        f"Kỳ Thủ Thắng: {winner}",
        f"Kỳ Thủ Cầm Cam: {player_cam}",
        f"Kỳ Thủ Cầm Xanh: {player_xanh}",
        "",
    ]

    root_node = getattr(core_bridge, "root_node", None)
    if root_node and root_node.state:
        root_board, root_turn = _decode_position_py(root_node.state["position"])
        root_ply = root_node.ply
    else:
        root_board = Board()
        root_board.setup_initial()
        root_turn = Player.CAM
        root_ply = 0

    std_board = Board()
    std_board.setup_initial()

    # If starting position is non-standard, insert ASCII diagram
    from .lppn import to_lppn
    if to_lppn(root_board, root_turn, ply=0) != to_lppn(std_board, Player.CAM, ply=0):
        lines.append(format_ascii_board(root_board, root_turn, root_ply))
        lines.append("")

    active_func = getattr(core_bridge, "_active_line_nodes", None)
    mainline_nodes = active_func() if callable(active_func) else ([root_node] if root_node else [])

    sim_board = root_board.clone()
    from . import rules

    # Mainline moves (one per line)
    for idx, node in enumerate(mainline_nodes[1:], 1):
        move_str = _format_move_token(node, sim_board, include_source=True)
        lines.append(f"{idx}.{move_str}")
        if node.move:
            # Replay on sim_board
            m_kind = node.move.get("kind")
            m_src = tuple(node.move["src"]) if node.move.get("src") else None
            m_dst = tuple(node.move["dst"]) if node.move.get("dst") else None
            m_ptype = node.move.get("ptype")
            m_facing = node.move.get("facing")
            m_rot = node.move.get("rotate_after", 0) % 4
            from .pieces import PieceType
            pt = PieceType[m_ptype] if m_ptype else None
            mv_obj = rules.Move(kind=m_kind, src=m_src, dst=m_dst, ptype=pt, facing=m_facing, rotate_after=m_rot)
            try:
                rules.apply_move(sim_board, sim_board.sq(*m_src).top.owner if m_src else (root_turn if idx % 2 != 0 else rules.other(root_turn)), mv_obj)
            except Exception:
                pass

    last_main = mainline_nodes[-1] if len(mainline_nodes) > 1 else None
    if last_main and getattr(last_main, "result", None):
        res_str = last_main.result.lower()
        if "chiếu hết" in res_str or "chieu het" in res_str or "checkmate" in res_str:
            lines.append(f"{len(mainline_nodes)}.#")
        elif any(k in res_str for k in ("xin thua", "xin hang", "resign")):
            lines.append(f"{len(mainline_nodes)}.Xin thua")

    # Variations (Biến nhánh)
    if root_node:
        all_nodes = root_node.all_nodes()
        main_set = set(id(n) for n in mainline_nodes)
        for node in all_nodes:
            if len(node.children) > 1:
                # Branches diverge at node.children[1:]
                for br_child in node.children[1:]:
                    lines.append("")
                    lines.append(f"nhánh: nước thứ {br_child.ply}")
                    # Follow branch chain
                    curr = br_child
                    b_idx = br_child.ply
                    while curr:
                        br_move_str = _format_move_token(curr, sim_board, include_source=True)
                        lines.append(f"{b_idx} {br_move_str}")
                        b_idx += 1
                        curr = curr.children[0] if curr.children else None

    lines.append("")
    return "\n".join(lines)


def export_to_txt_string(core_bridge: Any, metadata: Optional[Dict[str, Any]] = None) -> str:
    """Exports the entire MoveTree structure to standard 6-column 238-sample TXT format."""
    from .board import Board
    from .pieces import Player
    from .core_bridge import _decode_position_py

    meta = dict(metadata or {})
    play_time = meta.get("play_time") or meta.get("date") or datetime.now().strftime("%d-%m-%Y %H:%M")
    room_name = meta.get("room_name") or meta.get("room") or "Phòng tập luyện"
    winner = meta.get("winner") or "Chưa kết thúc"
    player_cam = meta.get("player_cam") or "Cam"
    player_xanh = meta.get("player_xanh") or "Xanh"

    lines = [
        "Kỳ Phổ Game Đấu",
        f"Tên Phòng: {room_name}",
        f"Ngày Giờ Chơi: {play_time}",
        f"Kỳ Thủ Thắng: {winner}",
        f"Kỳ Thủ Cầm Cam: {player_cam}",
        f"Kỳ Thủ Cầm Xanh: {player_xanh}",
        "",
    ]

    root_node = getattr(core_bridge, "root_node", None)
    if root_node and root_node.state:
        root_board, root_turn = _decode_position_py(root_node.state["position"])
        root_ply = root_node.ply
    else:
        root_board = Board()
        root_board.setup_initial()
        root_turn = Player.CAM
        root_ply = 0

    std_board = Board()
    std_board.setup_initial()

    # If starting position is non-standard, insert ASCII diagram
    from .lppn import to_lppn
    if to_lppn(root_board, root_turn, ply=0) != to_lppn(std_board, Player.CAM, ply=0):
        lines.append(format_ascii_board(root_board, root_turn, root_ply))
        lines.append("")

    active_func = getattr(core_bridge, "_active_line_nodes", None)
    mainline_nodes = active_func() if callable(active_func) else ([root_node] if root_node else [])

    sim_board = root_board.clone()

    # Clean mainline moves without (source)
    moves = []
    for idx, node in enumerate(mainline_nodes[1:], 1):
        clean_str = _format_move_token(node, sim_board, include_source=False)
        moves.append((idx, clean_str))

    last_main = mainline_nodes[-1] if len(mainline_nodes) > 1 else None
    if last_main and getattr(last_main, "result", None):
        res_str = last_main.result.lower()
        if "chiếu hết" in res_str or "chieu het" in res_str or "checkmate" in res_str:
            moves.append((len(moves) + 1, "#"))
        elif any(k in res_str for k in ("xin thua", "xin hang", "resign")):
            moves.append((len(moves) + 1, "Xin thua"))

    # 6 columns, 18 characters fixed width, 1 space for moves 1..9, no space for 10+
    cols = 6
    for i in range(0, len(moves), cols):
        chunk = moves[i : i + cols]
        row_parts = []
        for move_num, mv in chunk:
            entry = f" {move_num}.{mv}" if move_num <= 9 else f"{move_num}.{mv}"
            row_parts.append(f"{entry:<18}")
        lines.append("".join(row_parts).rstrip())

    # Variations (Biến nhánh)
    if root_node:
        all_nodes = root_node.all_nodes()
        for node in all_nodes:
            if len(node.children) > 1:
                for br_child in node.children[1:]:
                    lines.append("")
                    lines.append(f"nhánh: nước thứ {br_child.ply}")
                    br_moves = []
                    curr = br_child
                    b_idx = br_child.ply
                    while curr:
                        c_str = _format_move_token(curr, sim_board, include_source=False)
                        br_moves.append((b_idx, c_str))
                        b_idx += 1
                        curr = curr.children[0] if curr.children else None

                    for bi in range(0, len(br_moves), cols):
                        b_chunk = br_moves[bi : bi + cols]
                        b_parts = []
                        for m_n, m_v in b_chunk:
                            b_ent = f" {m_n}.{m_v}" if m_n <= 9 else f"{m_n}.{m_v}"
                            b_parts.append(f"{b_ent:<18}")
                        lines.append("".join(b_parts).rstrip())

    lines.append("")
    return "\n".join(lines)


def export_to_lpgf_string(core_bridge: Any, metadata: Optional[Dict[str, Any]] = None) -> str:
    """Delegates to export_to_lpgf_v2_string for standard LPGF 2.0 format."""
    return export_to_lpgf_v2_string(core_bridge, metadata)

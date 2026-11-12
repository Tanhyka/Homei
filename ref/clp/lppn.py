"""LPPN v0.2 position codec for Co Long Phuong."""
from __future__ import annotations

from collections import Counter
from dataclasses import dataclass

from .board import Board, SIZE
from .pieces import Piece, PieceType, Player, PIECE_CODE


MAGIC = "lppn1"
MAX_TEXT_LENGTH = 8192
MAX_STACK = 38
CODE_TO_TYPE = {code: piece_type for piece_type, code in PIECE_CODE.items()}
CANONICAL_TYPES = tuple(PieceType)


class LPPNError(ValueError):
    """Position encoding or decoding error."""


@dataclass(frozen=True)
class LPPNPosition:
    board: Board
    turn: Player
    ply: int


def _ensure_ply(ply: int) -> None:
    if type(ply) is not int or not 0 <= ply <= 1_000_000:
        raise LPPNError("Ply out of range")


def _piece_token(piece: Piece) -> str:
    if not isinstance(piece, Piece) or not isinstance(piece.ptype, PieceType) or not isinstance(piece.owner, Player):
        raise LPPNError("Invalid LPPN piece token")
    if type(piece.facing) is not int or piece.facing not in range(4):
        raise LPPNError("Invalid LPPN piece token")
    letter = PIECE_CODE[piece.ptype]
    return (letter if piece.owner is Player.CAM else letter.lower()) + str(piece.facing)


def _validate_board(board: Board, strict: bool) -> None:
    if not isinstance(board, Board) or len(board.grid) != SIZE or any(len(row) != SIZE for row in board.grid):
        raise LPPNError("Must have 9 rows")
    kings = Counter()
    total = 0
    for row in board.grid:
        for sq in row:
            if not hasattr(sq, "stack") or not isinstance(sq.stack, list):
                raise LPPNError("Malformed stack")
            st = sq.stack
            if len(st) > MAX_STACK:
                raise LPPNError("Stack exceeds limit")
            if st:
                _piece_token(st[0])
                owner = st[0].owner
                for piece in st:
                    _piece_token(piece)
                    if piece.owner is not owner:
                        raise LPPNError("Mixed-owner stack")
                    if piece.ptype is PieceType.VUONG:
                        kings[piece.owner] += 1
                if len(st) > 1 and any(p.ptype is PieceType.VUONG for p in st):
                    raise LPPNError("VUONG stacked")
                total += len(st)
    for field in ("reserve", "prisoners"):
        hands = getattr(board, field, None)
        if not isinstance(hands, dict) or set(hands) != set(Player):
            raise LPPNError("LPPN hands missing CAM/XANH")
        for owner in Player:
            types = hands[owner]
            if not isinstance(types, list):
                raise LPPNError("Invalid hand symbol")
            for ptype in types:
                if not isinstance(ptype, PieceType) or ptype is PieceType.VUONG:
                    raise LPPNError("VUONG in hand")
                if field == "reserve" and ptype not in (PieceType.LONG, PieceType.PHUONG):
                    raise LPPNError("Invalid reserve type")
            if field == "reserve" and len(set(types)) != len(types):
                raise LPPNError("Duplicate reserve piece")
            total += len(types)
    if strict:
        if kings[Player.CAM] != 1 or kings[Player.XANH] != 1:
            raise LPPNError("Strict LPPN requires one VUONG per side")
        if total > 40:
            raise LPPNError("Over 40 pieces")


def _hand_text(hands: dict) -> str:
    def one(player: Player) -> str:
        counts = Counter(hands[player])
        text = "".join(PIECE_CODE[ptype] * counts[ptype] for ptype in CANONICAL_TYPES)
        return text or "-"
    return one(Player.CAM) + "/" + one(Player.XANH)


def to_lppn(board: Board, turn: Player, *, ply: int = 0, strict: bool = False) -> str:
    """Serialize a CLP Board to canonical LPPN (wire format lppn1)."""
    if not isinstance(turn, Player):
        raise LPPNError("turn must be Player.CAM or Player.XANH")
    _ensure_ply(ply)
    _validate_board(board, strict)
    rows = []
    for row in board.grid:
        parts = []
        empty = 0
        for sq in row:
            stack = sq.stack
            if not stack:
                empty += 1
                continue
            if empty:
                parts.append(str(empty))
                empty = 0
            parts.append(
                _piece_token(stack[0]) if len(stack) == 1
                else "[" + "".join(_piece_token(piece) for piece in stack) + "]"
            )
        if empty:
            parts.append(str(empty))
        rows.append("".join(parts))
    side = "c" if turn is Player.CAM else "x"
    result = f"{MAGIC} {'/'.join(rows)} {side} {_hand_text(board.reserve)} {_hand_text(board.prisoners)} {ply}"
    if len(result.encode("utf-8")) > MAX_TEXT_LENGTH:
        raise LPPNError("LPPN too long")
    return result


def _decode_pair(pair: str) -> Piece:
    if len(pair) != 2 or pair[1] not in "0123" or not pair[0].isalpha() or not pair[0].isascii():
        raise LPPNError("Invalid LPPN piece token")
    upper = pair[0].upper()
    if upper not in CODE_TO_TYPE:
        raise LPPNError("Bad piece type in LPPN")
    side = Player.CAM if pair[0].isupper() else Player.XANH
    return Piece(CODE_TO_TYPE[upper], side, int(pair[1]))


def _parse_row(row: str, row_index: int, board: Board) -> None:
    col = 0
    i = 0
    while i < len(row):
        ch = row[i]
        if "1" <= ch <= "9":
            col += int(ch)
            i += 1
        else:
            if col >= SIZE:
                raise LPPNError("Row too wide")
            if ch == "[":
                end = row.find("]", i + 1)
                if end < 0 or (end - i - 1) < 4 or (end - i - 1) % 2 != 0:
                    raise LPPNError("Malformed stack")
                pieces = [_decode_pair(row[n:n+2]) for n in range(i + 1, end, 2)]
                i = end + 1
            else:
                if i + 2 > len(row):
                    raise LPPNError("Truncated piece")
                pieces = [_decode_pair(row[i:i+2])]
                i += 2
            board.sq(row_index, col).stack = pieces
            col += 1
        if col > SIZE:
            raise LPPNError("Row exceeds 9 squares")
    if col != SIZE:
        raise LPPNError("Row must contain 9 squares")


def _parse_hands(text: str, reserve: bool) -> dict[Player, list[PieceType]]:
    halves = text.split("/")
    if len(halves) != 2:
        raise LPPNError("LPPN hands missing CAM/XANH")
    out: dict[Player, list[PieceType]] = {}
    for owner, chars in zip((Player.CAM, Player.XANH), halves):
        if chars == "-":
            out[owner] = []
            continue
        if not chars:
            raise LPPNError("Empty hand token")
        items = []
        for ch in chars:
            if not ch.isupper() or not ch.isascii():
                raise LPPNError("Invalid hand symbol")
            if ch not in CODE_TO_TYPE:
                raise LPPNError("Bad piece type in LPPN")
            ptype = CODE_TO_TYPE[ch]
            if ptype is PieceType.VUONG:
                raise LPPNError("VUONG in hand")
            if reserve and ptype not in (PieceType.LONG, PieceType.PHUONG):
                raise LPPNError("Invalid reserve type")
            if reserve and ptype in items:
                raise LPPNError("Duplicate reserve piece")
            items.append(ptype)
        out[owner] = items
    return out


def from_lppn(text: str, *, strict: bool = False) -> LPPNPosition:
    """Parse LPPN into a new independent CLP Board, side and ply counter."""
    if not isinstance(text, str) or len(text.encode("utf-8")) > MAX_TEXT_LENGTH:
        raise LPPNError("LPPN too long")
    fields = text.strip().split()
    if len(fields) != 6 or fields[0] != MAGIC or fields[2] not in ("c", "x"):
        raise LPPNError("Expected lppn1 BOARD TURN RESERVE PRISONERS PLY")
    _, encoded_board, side_text, reserve_text, prisoners_text, ply_text = fields
    if not ply_text.isascii() or not ply_text.isdecimal() or (len(ply_text) > 1 and ply_text[0] == "0"):
        raise LPPNError("Invalid ply")
    ply = int(ply_text)
    _ensure_ply(ply)
    board = Board()
    board.reserve = _parse_hands(reserve_text, reserve=True)
    board.prisoners = _parse_hands(prisoners_text, reserve=False)
    rows = encoded_board.split("/")
    if len(rows) != SIZE:
        raise LPPNError("Must have 9 rows")
    for row_index, row in enumerate(rows):
        _parse_row(row, row_index, board)
    _validate_board(board, strict)
    return LPPNPosition(board=board, turn=Player.CAM if side_text == "c" else Player.XANH, ply=ply)


def normalize_lppn(text: str, *, strict: bool = False) -> str:
    decoded = from_lppn(text, strict=strict)
    return to_lppn(decoded.board, decoded.turn, ply=decoded.ply, strict=strict)

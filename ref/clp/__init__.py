"""Core reference package for Homei (rules, board, LPPN, CoreBridge, RecordIO)."""

from .board import Board, SIZE, Square
from .core_bridge import CoreBridge, DisplayBoard, DisplayCell
from .engine_client import EngineClient, EngineIteration
from .lppn import LPPNError, LPPNPosition, from_lppn, normalize_lppn, to_lppn
from .native import CoreUnavailable, NativeCore, native_core
from .pieces import NON_ROTATABLE, PIECE_CODE, Piece, PieceType, Player, initial_facing
from .record_io import ImportedRecord, RecordIO, RecordKind
from .rules import Move, apply_move, game_status, generate_moves, is_attacked, is_in_check, legal_moves, other

__all__ = [
    "Board",
    "CoreBridge",
    "CoreUnavailable",
    "DisplayBoard",
    "DisplayCell",
    "EngineClient",
    "EngineIteration",
    "ImportedRecord",
    "LPPNError",
    "LPPNPosition",
    "Move",
    "NON_ROTATABLE",
    "NativeCore",
    "PIECE_CODE",
    "Piece",
    "PieceType",
    "Player",
    "RecordIO",
    "RecordKind",
    "SIZE",
    "Square",
    "apply_move",
    "from_lppn",
    "game_status",
    "generate_moves",
    "initial_facing",
    "is_attacked",
    "is_in_check",
    "legal_moves",
    "native_core",
    "normalize_lppn",
    "other",
    "to_lppn",
]

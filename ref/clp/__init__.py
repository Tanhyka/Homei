"""Core package for ColongphuongGUI_train (Python port of CLP_GUI_TRAINING with native cpp-CLP acceleration)."""

from .analysis_session import AnalysisSession
from .board import Board, SIZE, Square
from .core_bridge import CoreBridge, DisplayBoard, DisplayCell
from .engine_client import EngineClient, EngineIteration
from .lppn import LPPNError, LPPNPosition, from_lppn, normalize_lppn, to_lppn
from .native import CoreUnavailable, NativeCore, native_core
from .pieces import NON_ROTATABLE, PIECE_CODE, Piece, PieceType, Player, initial_facing
from .record_io import ImportedRecord, RecordIO, RecordKind
from .rule_move_policy import (
    MoveReason,
    RuleMoveDecision,
    RuleMovePolicy,
    RuleMoveSelection,
    TeachingDecision,
)
from .rules import Move, apply_move, game_status, generate_moves, is_attacked, is_in_check, legal_moves, other
from .session_recovery import SessionRecovery
from .turn_coordinator import (
    BudgetMode,
    Capabilities,
    Command,
    Decision,
    MatchConfig,
    Phase,
    SearchBudget,
    Seat,
    Side,
    SideConfig,
    TurnCoordinator,
)

__all__ = [
    "AnalysisSession",
    "Board",
    "BudgetMode",
    "Capabilities",
    "Command",
    "CoreBridge",
    "CoreUnavailable",
    "Decision",
    "DisplayBoard",
    "DisplayCell",
    "EngineClient",
    "EngineIteration",
    "ImportedRecord",
    "LPPNError",
    "LPPNPosition",
    "MatchConfig",
    "Move",
    "MoveReason",
    "NON_ROTATABLE",
    "NativeCore",
    "PIECE_CODE",
    "Phase",
    "Piece",
    "PieceType",
    "Player",
    "RecordIO",
    "RecordKind",
    "RuleMoveDecision",
    "RuleMovePolicy",
    "RuleMoveSelection",
    "SIZE",
    "SearchBudget",
    "Seat",
    "SessionRecovery",
    "Side",
    "SideConfig",
    "Square",
    "TeachingDecision",
    "TurnCoordinator",
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

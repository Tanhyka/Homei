"""STEP 4 RuleMovePolicy: Python port of CLP_GUI_TRAINING/src/rule_move_policy.h & .cpp."""

from __future__ import annotations

import copy
from dataclasses import dataclass, field
from enum import Enum
import secrets
from typing import List


class MoveReason(Enum):
    Random = 0
    Attack = 1
    Defense = 2
    Mate = 3
    Even = 4
    Book = 5


@dataclass
class TeachingDecision:
    position: dict = field(default_factory=dict)
    move: dict = field(default_factory=dict)
    judgement: str = ""
    source: str = ""


@dataclass
class RuleMoveDecision:
    legalIndex: int = -1
    reason: MoveReason = MoveReason.Random
    blocked: bool = False
    source: str = "Core"
    detail: str = "Allowed"


@dataclass
class RuleMoveSelection:
    moves: List[RuleMoveDecision] = field(default_factory=list)
    error: str = ""

    def chooseRandom(self) -> int:
        if self.error:
            return -1
        permitted = [m.legalIndex for m in self.moves if not m.blocked]
        if not permitted:
            return -1
        return permitted[secrets.randbelow(len(permitted))]


def _move_key(value: dict) -> dict:
    d = copy.deepcopy(value) if isinstance(value, dict) else {}
    d.pop("notation", None)
    return d


def _state_key(value: dict) -> dict:
    d = copy.deepcopy(value) if isinstance(value, dict) else {}
    d.pop("notation", None)
    return d


def _label(value: str) -> str:
    return " ".join(value.strip().split()).casefold()


class RuleMovePolicy:
    @staticmethod
    def reasonName(reason: MoveReason) -> str:
        mapping = {
            MoveReason.Random: "Random",
            MoveReason.Attack: "Attack",
            MoveReason.Defense: "Defense",
            MoveReason.Mate: "Mate",
            MoveReason.Even: "Even",
            MoveReason.Book: "Book",
        }
        return mapping.get(reason, "Random")

    @staticmethod
    def evaluate(
        position: dict,
        legal: List[dict],
        teaching: List[TeachingDecision],
    ) -> RuleMoveSelection:
        result = RuleMoveSelection()
        pos_k = _state_key(position)
        for i, leg in enumerate(legal):
            move_dec = RuleMoveDecision(legalIndex=i)
            leg_k = _move_key(leg)
            for record in teaching:
                if _state_key(record.position) != pos_k or _move_key(record.move) != leg_k:
                    continue
                judgement = _label(record.judgement)
                if judgement in ("blunder", "bad", "not good"):
                    move_dec.blocked = True
                    move_dec.source = "Core + Teaching"
                    move_dec.detail = "Blocked: " + record.judgement
                elif judgement not in ("good", "don't know"):
                    result.error = f"Undefined Teaching judgement: {record.judgement} ({record.source})"
            result.moves.append(move_dec)

        allowed = any(not m.blocked for m in result.moves)
        if legal and not allowed and not result.error:
            result.error = "Teaching policy conflict: all legal moves are blocked."
        return result

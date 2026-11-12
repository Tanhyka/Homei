"""STEP 5 TurnCoordinator: Python port of CLP_GUI_TRAINING/include/step5/turn_coordinator.h & .cpp."""

from __future__ import annotations

import copy
from dataclasses import dataclass, field
from enum import Enum


class Side(Enum):
    Cam = 0
    Xanh = 1


class Seat(Enum):
    Human = 0
    Engine = 1


class BudgetMode(Enum):
    Infinite = 0
    TimeMs = 1
    Nodes = 2
    Depth = 3


class Phase(Enum):
    Idle = 0
    WaitingHuman = 1
    WaitingEngine = 2
    Stopping = 3
    Completed = 4
    Error = 5


class Command(Enum):
    None_ = 0
    WaitHuman = 1
    Search = 2
    StopSearch = 3
    GameOver = 4
    Reject = 5


setattr(Command, "None", Command.None_)


@dataclass
class SearchBudget:
    mode: BudgetMode = BudgetMode.TimeMs
    value: int = 1000


@dataclass
class Capabilities:
    ready: bool = False
    stop: bool = False
    infinite: bool = False
    movetime: bool = False
    nodes: bool = False
    depth: bool = False


@dataclass
class SideConfig:
    seat: Seat = Seat.Human
    engine: Capabilities = field(default_factory=Capabilities)


@dataclass
class MatchConfig:
    cam: SideConfig = field(default_factory=SideConfig)
    xanh: SideConfig = field(default_factory=SideConfig)
    budget: SearchBudget = field(default_factory=SearchBudget)


@dataclass
class Decision:
    command: Command = Command.None_
    side: Side = Side.Cam
    requestToken: int = 0
    message: str = ""


class TurnCoordinator:
    def __init__(self):
        self._cfg: MatchConfig = MatchConfig()
        self._phase: Phase = Phase.Idle
        self._to_move: Side = Side.Cam
        self._serial: int = 0
        self._pending_token: int = 0

    def _rejected(self, msg: str) -> Decision:
        return Decision(Command.Reject, self._to_move, self._pending_token, msg)

    def _side_config(self, s: Side) -> SideConfig:
        return self._cfg.cam if s == Side.Cam else self._cfg.xanh

    def _next_turn(self, terminal: bool) -> Decision:
        self._pending_token = 0
        if terminal:
            self._phase = Phase.Completed
            return Decision(Command.GameOver, self._to_move, 0, "Game ended by authoritative Core")
        seat = self._side_config(self._to_move)
        if seat.seat == Seat.Human:
            self._phase = Phase.WaitingHuman
            return Decision(Command.WaitHuman, self._to_move, 0, "")
        cap = seat.engine
        if not cap.ready or not cap.stop:
            self._phase = Phase.Error
            return self._rejected("Engine unavailable or has no Stop capability")
        mode = self._cfg.budget.mode
        val = self._cfg.budget.value
        if mode == BudgetMode.TimeMs:
            if not cap.movetime or val < 1 or val > 86400000:
                self._phase = Phase.Error
                return self._rejected("Unsupported movetime or invalid milliseconds")
        elif mode == BudgetMode.Nodes:
            if not cap.nodes or val == 0:
                self._phase = Phase.Error
                return self._rejected("Nodes budget not advertised by engine")
        elif mode == BudgetMode.Depth:
            if not cap.depth or val == 0:
                self._phase = Phase.Error
                return self._rejected("Depth budget not advertised by engine")
        elif mode == BudgetMode.Infinite:
            self._phase = Phase.Error
            return self._rejected("Continuous Infinite needs an explicit move-commit policy; no hidden time cutoff")

        self._phase = Phase.WaitingEngine
        self._serial += 1
        self._pending_token = self._serial
        return Decision(Command.Search, self._to_move, self._pending_token, "")

    def start(self, config: MatchConfig, toMove: Side, terminal: bool = False) -> Decision:
        if self._phase in (Phase.WaitingHuman, Phase.WaitingEngine, Phase.Stopping):
            return self._rejected("Session already active")
        self._cfg = copy.deepcopy(config)
        self._to_move = toMove
        return self._next_turn(terminal)

    def acceptedHumanMove(self, nextToMove: Side, terminal: bool = False) -> Decision:
        if self._phase != Phase.WaitingHuman:
            return self._rejected("No human move is expected")
        self._to_move = nextToMove
        return self._next_turn(terminal)

    def acceptedEngineMove(self, requestToken: int, nextToMove: Side, terminal: bool = False) -> Decision:
        if self._phase != Phase.WaitingEngine or requestToken == 0 or requestToken != self._pending_token:
            return self._rejected("Stale, duplicate or unrequested engine result")
        self._to_move = nextToMove
        return self._next_turn(terminal)

    def stop(self) -> Decision:
        if self._phase == Phase.WaitingEngine:
            old_token = self._pending_token
            self._pending_token = 0
            self._phase = Phase.Stopping
            return Decision(Command.StopSearch, self._to_move, old_token, "Stop continuous session; never commit delayed bestmove")
        if self._phase == Phase.WaitingHuman:
            self._pending_token = 0
            self._phase = Phase.Idle
            return Decision(Command.None_, self._to_move, 0, "Stopped")
        if self._phase == Phase.Stopping:
            return self._rejected("Already stopping")
        self._phase = Phase.Idle
        self._pending_token = 0
        return Decision(Command.None_, self._to_move, 0, "Already stopped")

    def stopAcknowledged(self) -> Decision:
        if self._phase != Phase.Stopping:
            return self._rejected("No stop is pending")
        self._phase = Phase.Idle
        return Decision(Command.None_, self._to_move, 0, "Stopped")

    def engineFailure(self, why: str) -> Decision:
        if self._phase not in (Phase.WaitingEngine, Phase.Stopping):
            return self._rejected("No active engine request")
        self._phase = Phase.Error
        self._pending_token = 0
        return Decision(Command.Reject, self._to_move, 0, "Engine failure: " + why)

    def externalPositionChanged(self) -> Decision:
        if self._phase == Phase.WaitingEngine:
            return self.stop()
        if self._phase == Phase.Stopping:
            return self._rejected("Stop already pending")
        self._phase = Phase.Idle
        self._pending_token = 0
        return Decision(Command.None_, self._to_move, 0, "Session canceled by position change")

    def phase(self) -> Phase:
        return self._phase

    def sideToMove(self) -> Side:
        return self._to_move

    def pendingRequest(self) -> int:
        return self._pending_token

    def active(self) -> bool:
        return self._phase in (Phase.WaitingHuman, Phase.WaitingEngine)

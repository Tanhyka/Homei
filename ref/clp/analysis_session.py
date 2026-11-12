"""AnalysisSession: UI session state (Python port of CLP_GUI_TRAINING/src/analysis_session.h)."""

from __future__ import annotations

import copy
from enum import Enum
from typing import Callable, Optional

from PyQt6.QtCore import QElapsedTimer

from .engine_client import EngineIteration


class AnalysisSession:
    class State(Enum):
        Idle = 0
        Running = 1
        Stopping = 2
        Finished = 3

    def __init__(self, now_fn: Optional[Callable[[], int]] = None):
        self._now_fn = now_fn
        self._clock = QElapsedTimer()
        self._clock.start()
        self._state = AnalysisSession.State.Idle
        self._request: int = 0
        self._started_at: int = 0
        self._frozen_ms: int = 0
        self._latest: Optional[EngineIteration] = None
        self._reason: str = ""

    def _now(self) -> int:
        return int(self._now_fn()) if self._now_fn is not None else int(self._clock.elapsed())

    def started(self, ready: bool, infiniteSupported: bool, request: int) -> bool:
        if not ready or not infiniteSupported or self.active() or request <= 0:
            return False
        self._request = request
        self._started_at = self._now()
        self._frozen_ms = 0
        self._latest = None
        self._reason = ""
        self._state = AnalysisSession.State.Running
        return True

    def requestStop(self) -> bool:
        if self._state != AnalysisSession.State.Running:
            return False
        self._state = AnalysisSession.State.Stopping
        return True

    def finish(self, request: int, reason: str) -> bool:
        if not self.active() or request != self._request:
            return False
        self._frozen_ms = self.elapsedMs()
        self._state = AnalysisSession.State.Finished
        self._reason = reason
        return True

    def update(self, info: EngineIteration) -> bool:
        if not self.active() or info.request != self._request:
            return False
        if info.rank == 1:
            self._latest = copy.deepcopy(info)
        return True

    def active(self) -> bool:
        return self._state in (AnalysisSession.State.Running, AnalysisSession.State.Stopping)

    def state(self) -> "AnalysisSession.State":
        return self._state

    def request(self) -> int:
        return self._request

    def elapsedMs(self) -> int:
        return max(0, self._now() - self._started_at) if self.active() else self._frozen_ms

    def latest(self) -> Optional[EngineIteration]:
        return self._latest

    def reason(self) -> str:
        return self._reason

    def summary(self) -> str:
        seconds = self.elapsedMs() // 1000
        h = seconds // 3600
        m = (seconds // 60) % 60
        s = seconds % 60
        time_str = f"{h:02d}:{m:02d}:{s:02d}"
        depth_str = str(self._latest.depth) if self._latest is not None else "—"
        nodes_str = str(self._latest.nodes) if self._latest is not None else "—"
        nps_str = str(self._latest.nps) if self._latest is not None else "—"
        return f"Time {time_str}   Depth {depth_str}   Nodes {nodes_str}   NPS {nps_str}"

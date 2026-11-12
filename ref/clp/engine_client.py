"""EngineClient: Async CLP-EP v1 protocol client (Python port of CLP_GUI_TRAINING/src/engine_client)."""

from __future__ import annotations

import base64
from dataclasses import dataclass, field
import json
from pathlib import Path
from typing import List, Set

from PyQt6.QtCore import QObject, QProcess, QTimer, pyqtSignal


def _b64url_decode(token: str) -> bytes:
    cleaned = token.strip()
    if not cleaned:
        raise ValueError("Empty base64url token")
    pad = "=" * ((-len(cleaned)) % 4)
    return base64.urlsafe_b64decode(cleaned + pad)


@dataclass
class EngineIteration:
    request: int = 0
    rank: int = 1
    depth: int = 0
    nodes: int = 0
    timeMs: int = 0
    nps: int = 0
    score: int = 0
    centipawns: bool = False
    scoreText: str = ""
    pvText: str = ""
    pv: List[dict] = field(default_factory=list)


class EngineClient(QObject):
    infiniteStarted = pyqtSignal(int)
    bestMove = pyqtSignal(bytes)
    message = pyqtSignal(str)
    engineReady = pyqtSignal(bool)
    iteration = pyqtSignal(object)  # EngineIteration
    completed = pyqtSignal(str)

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self.process_ = QProcess(self)
        self.startupTimeout_ = QTimer(self)
        self.shutdownTimeout_ = QTimer(self)
        self.startupTimeout_.setSingleShot(True)
        self.shutdownTimeout_.setSingleShot(True)

        self.pendingExecutable_: str = ""
        self.launchGeneration_: int = 0
        self.shuttingDown_: bool = False
        self.awaitingOptions_: bool = False
        self.acknowledgedOptions_: Set[str] = set()
        self.partial_: bytearray = bytearray()
        self.executable_: str = ""
        self.engineName_: str = ""

        self.capMovetime_: bool = False
        self.capSearch_: bool = False
        self.capInfinite_: bool = False
        self.capStop_: bool = False
        self.pendingInfinite_: bool = False
        self.startedInfinite_: bool = False
        self.ready_: bool = False
        self.handshaken_: bool = False
        self.pendingSearch_: bool = False
        self.protocol_: bool = False
        self.capDepth_: bool = False
        self.capNodes_: bool = False
        self.capMultiPV_: bool = False
        self.confirmedOptions_: int = 0
        self.nextId_: int = 0
        self.activeId_: int = 0
        self.pendingMillis_: int = 0
        self.pendingDepth_: int = 0
        self.pendingNodes_: int = 0
        self.pendingMultipv_: int = 1

        self.startupTimeout_.timeout.connect(self._on_startup_timeout)
        self.shutdownTimeout_.timeout.connect(self._on_shutdown_timeout)
        self.process_.started.connect(self._on_started)
        self.process_.setProcessChannelMode(QProcess.ProcessChannelMode.SeparateChannels)
        self.process_.readyReadStandardOutput.connect(self.onOutput)
        self.process_.readyReadStandardError.connect(self._on_stderr)
        self.process_.errorOccurred.connect(self._on_error)
        self.process_.finished.connect(self._on_finished)

    def _on_startup_timeout(self):
        self.message.emit("Engine process error: startup/CLP-EP handshake timed out")
        self.shutdown()

    def _on_shutdown_timeout(self):
        if self.process_.state() != QProcess.ProcessState.NotRunning:
            self.process_.kill()

    def _on_started(self):
        if self.shuttingDown_:
            self.process_.kill()
            return
        self.send("clp 1")
        self.message.emit("CLP-EP v1: waiting for handshake")

    def _on_stderr(self):
        s = bytes(self.process_.readAllStandardError()).decode("utf-8", errors="replace").strip()
        if s:
            self.message.emit("Engine stderr: " + s[:300])

    def _on_error(self, _err):
        if self.shuttingDown_:
            return
        self.message.emit("Engine process error: " + self.process_.errorString())
        self.resetState()
        self.engineReady.emit(False)

    def _on_finished(self, code: int, _status):
        expected = self.shuttingDown_
        self.shutdownTimeout_.stop()
        self.shuttingDown_ = False
        self.resetState()
        if not expected:
            self.message.emit(f"Engine exited ({code})")
        self.engineReady.emit(False)
        nxt = self.pendingExecutable_
        self.pendingExecutable_ = ""
        generation = self.launchGeneration_
        if nxt:
            QTimer.singleShot(0, self, lambda: self.launch(nxt) if generation == self.launchGeneration_ else None)

    def ready(self) -> bool:
        return self.ready_

    def busy(self) -> bool:
        return self.activeId_ > 0

    def engineName(self) -> str:
        return self.engineName_

    def supportsMovetime(self) -> bool:
        return self.ready_ and self.capMovetime_

    def supportsStop(self) -> bool:
        return self.ready_ and self.capStop_

    def supportsNodes(self) -> bool:
        return self.ready_ and self.capNodes_

    def supportsDepth(self) -> bool:
        return self.ready_ and self.capDepth_

    def supportsInfinite(self) -> bool:
        return self.ready_ and self.capInfinite_ and self.capStop_

    def resetState(self):
        self.startupTimeout_.stop()
        self.ready_ = False
        self.handshaken_ = False
        self.activeId_ = 0
        self.pendingSearch_ = False
        self.partial_.clear()
        self.protocol_ = False
        self.capDepth_ = False
        self.capNodes_ = False
        self.capMultiPV_ = False
        self.confirmedOptions_ = 0
        self.awaitingOptions_ = False
        self.acknowledgedOptions_.clear()
        self.capMovetime_ = False
        self.capSearch_ = False
        self.engineName_ = ""
        self.capInfinite_ = False
        self.capStop_ = False
        self.pendingInfinite_ = False
        self.startedInfinite_ = False

    def shutdown(self):
        self.launchGeneration_ += 1
        self.pendingExecutable_ = ""
        self.resetState()
        self.engineReady.emit(False)
        if self.process_.state() != QProcess.ProcessState.NotRunning:
            if not self.shuttingDown_:
                self.shuttingDown_ = True
                self.process_.write(b"quit\n")
                self.shutdownTimeout_.start(1000)

    def launch(self, executable: str):
        self.shutdown()
        if self.process_.state() != QProcess.ProcessState.NotRunning:
            self.pendingExecutable_ = executable
            return
        self.startProcess(executable)

    def startProcess(self, executable: str):
        self.shuttingDown_ = False
        self.shutdownTimeout_.stop()
        self.resetState()
        p = Path(executable)
        if not p.is_file():
            self.message.emit("Engine executable does not exist")
            return
        self.executable_ = str(p)
        self.process_.setProgram(self.executable_)
        self.process_.setWorkingDirectory(str(p.parent.resolve()))
        self.startupTimeout_.start(5000)
        self.process_.start()

    def send(self, command: str | bytes):
        if self.process_.state() == QProcess.ProcessState.Running:
            data = command.encode("utf-8") if isinstance(command, str) else command
            self.process_.write(data + b"\n")

    def onOutput(self):
        self.partial_.extend(bytes(self.process_.readAllStandardOutput()))
        if len(self.partial_) > 2 * 1024 * 1024:
            self.partial_.clear()
            self.message.emit("Engine line buffer limit exceeded")
            self.stop()
            return
        while True:
            end = self.partial_.find(b"\n")
            if end < 0:
                break
            raw_line = bytes(self.partial_[:end]).strip()
            del self.partial_[: end + 1]
            if raw_line:
                self.onLine(raw_line)

    def onLine(self, raw_line: bytes | str):
        if self.shuttingDown_:
            return
        line = raw_line.decode("utf-8", errors="replace").strip() if isinstance(raw_line, (bytes, bytearray)) else raw_line.strip()
        if not line:
            return
        if line == "capability search infinite":
            self.capInfinite_ = True
            return
        if line == "capability control stop":
            self.capStop_ = True
            return
        if line.startswith("id name "):
            self.engineName_ = line[8:]
            return
        if line == "capability search legal":
            self.capSearch_ = True
            return
        if line.startswith("capability clocks "):
            self.capMovetime_ = "movetime" in line.split(" ")
            return
        if line == "protocol clp-ep 1":
            self.protocol_ = True
            return
        if line.startswith("option name Depth type spin "):
            self.capDepth_ = True
            return
        if line.startswith("option name Nodes type spin "):
            self.capNodes_ = True
            return
        if line.startswith("option name MultiPV type spin "):
            self.capMultiPV_ = True
            return
        if line == "clpok":
            self.handshaken_ = True
            self.send("isready")
            return
        if line == "readyok":
            if (
                not self.handshaken_
                or not self.protocol_
                or not self.capMultiPV_
                or (not (self.capSearch_ and self.capMovetime_) and not (self.capDepth_ and self.capNodes_))
            ):
                self.message.emit("Unsupported CLP-EP: missing protocol/Depth/Nodes/MultiPV capabilities")
                return
            self.startupTimeout_.stop()
            self.ready_ = True
            self.engineReady.emit(True)
            self.message.emit("Engine ready (capabilities verified)")
            return
        if line.startswith("error "):
            err_text = line[:320]
            self.message.emit("CLP-EP error: " + err_text)
            self.pendingSearch_ = False
            self.pendingInfinite_ = False
            self.startedInfinite_ = False
            self.activeId_ = 0
            self.completed.emit("CLP-EP error: " + err_text)
            return
        if line.startswith("positionok "):
            if not self.pendingSearch_ or self.awaitingOptions_:
                return
            self.confirmedOptions_ = 0
            self.awaitingOptions_ = True
            self.acknowledgedOptions_.clear()
            if self.capDepth_ and not self.pendingInfinite_:
                self.send(f"setoption name Depth value {self.pendingDepth_}")
            if self.capNodes_ and not self.pendingInfinite_:
                self.send(f"setoption name Nodes value {self.pendingNodes_}")
            self.send(f"setoption name MultiPV value {self.pendingMultipv_}")
            return
        if line.startswith("optionok name "):
            if not self.pendingSearch_ or not self.awaitingOptions_:
                return
            response = line[14:].strip()
            name = ""
            for option in ("Depth", "Nodes", "MultiPV"):
                if response == option or response.startswith(option + " value "):
                    name = option
                    break
            if not name or name in self.acknowledgedOptions_:
                return
            if (name == "Depth" and not self.capDepth_) or (name == "Nodes" and not self.capNodes_):
                return
            if self.pendingInfinite_ and name != "MultiPV":
                return
            self.acknowledgedOptions_.add(name)
            self.confirmedOptions_ = len(self.acknowledgedOptions_)
            expected = 1 if self.pendingInfinite_ else (1 + int(self.capDepth_) + int(self.capNodes_))
            if self.confirmedOptions_ == expected:
                self.pendingSearch_ = False
                self.awaitingOptions_ = False
                if self.pendingInfinite_:
                    self.send(f"go id {self.activeId_} infinite")
                else:
                    self.send(f"go id {self.activeId_} movetime {self.pendingMillis_}")
            return
        if line.startswith("searchstarted id "):
            try:
                req_id = int(line[17:].strip())
            except ValueError:
                return
            if req_id == self.activeId_ and not self.pendingSearch_ and self.pendingInfinite_ and not self.startedInfinite_:
                self.startedInfinite_ = True
                self.infiniteStarted.emit(req_id)
            return
        if line.startswith("info id "):
            parts = line.split(" ")
            if len(parts) == 5 and parts[3] == "string64":
                try:
                    req_id = int(parts[2])
                    if req_id == self.activeId_:
                        decoded = json.loads(_b64url_decode(parts[4]).decode("utf-8"))
                        text = decoded.get("message", "") if isinstance(decoded, dict) else ""
                        if text:
                            self.message.emit(text)
                except Exception:
                    pass
                return
            if len(parts) != 5 or parts[3] != "info64":
                return
            try:
                req_id = int(parts[2])
            except ValueError:
                return
            if req_id != self.activeId_:
                return
            try:
                data = json.loads(_b64url_decode(parts[4]).decode("utf-8"))
            except Exception:
                return
            if not isinstance(data, dict) or data.get("schema") != "clp.info/1" or data.get("id") != self.activeId_:
                return
            info = EngineIteration()
            info.request = req_id
            info.rank = int(data.get("multipv", 1))
            info.depth = int(data.get("depth", 0))
            info.timeMs = int(data.get("time_ms", 0))
            info.nodes = int(data.get("nodes", 0))
            info.nps = (info.nodes * 1000 // info.timeMs) if info.timeMs > 0 else 0
            score = data.get("score", {}) if isinstance(data.get("score"), dict) else {}
            info.centipawns = score.get("kind") == "cp" and score.get("perspective") == "root"
            info.score = int(score.get("value", 0))
            info.scoreText = ("Mate " if score.get("kind") == "mate" else "") + str(info.score)
            pv_arr = data.get("pv", [])
            info.pv = pv_arr if isinstance(pv_arr, list) else []
            info.pvText = " ".join(json.dumps(m, separators=(",", ":")) for m in info.pv if isinstance(m, dict))
            self.iteration.emit(info)
            return
        if line.startswith("bestmove id "):
            parts = line.split(" ")
            if len(parts) < 4:
                return
            try:
                req_id = int(parts[2])
            except ValueError:
                return
            if req_id != self.activeId_:
                return
            self.activeId_ = 0
            self.pendingSearch_ = False
            self.awaitingOptions_ = False
            self.pendingInfinite_ = False
            self.startedInfinite_ = False
            if len(parts) == 5 and parts[3] == "move64":
                try:
                    self.bestMove.emit(_b64url_decode(parts[4]))
                except Exception:
                    pass
            self.completed.emit(line[:500])
            return

    def begin(self, state64: str, milliseconds: int, depth: int, nodes: int, multipv: int) -> bool:
        if (
            not self.ready_
            or not self.capMovetime_
            or self.activeId_ > 0
            or not state64
            or milliseconds < 1
            or milliseconds > 86400000
            or depth < 1
            or depth > 32
            or nodes < 1
            or nodes > 50000000
            or multipv < 1
            or multipv > 5
        ):
            return False
        self.nextId_ += 1
        self.activeId_ = self.nextId_
        self.pendingMillis_ = milliseconds
        self.pendingDepth_ = depth
        self.pendingNodes_ = nodes
        self.pendingMultipv_ = multipv
        self.pendingInfinite_ = False
        self.startedInfinite_ = False
        self.pendingSearch_ = True
        self.confirmedOptions_ = 0
        self.awaitingOptions_ = False
        self.acknowledgedOptions_.clear()
        self.send("position state64 " + state64)
        return True

    def beginInfinite(self, state64: str, multipv: int) -> bool:
        if not self.supportsInfinite() or self.activeId_ > 0 or not state64 or multipv < 1 or multipv > 3:
            return False
        self.nextId_ += 1
        self.activeId_ = self.nextId_
        self.pendingInfinite_ = True
        self.startedInfinite_ = False
        self.pendingMultipv_ = multipv
        self.pendingSearch_ = True
        self.awaitingOptions_ = False
        self.confirmedOptions_ = 0
        self.acknowledgedOptions_.clear()
        self.send("position state64 " + state64)
        return True

    def stop(self):
        if self.pendingSearch_:
            self.pendingSearch_ = False
            self.activeId_ = 0
            self.pendingInfinite_ = False
            self.startedInfinite_ = False
            self.completed.emit("Analysis stopped")
            return
        if self.activeId_ > 0:
            self.send(f"stop id {self.activeId_}")

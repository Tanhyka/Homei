"""Operational safety process manager and CLP-EP v1 client for Homei.

Enforces:
- Separate process group (CREATE_NEW_PROCESS_GROUP on Windows).
- Dedicated stdout and stderr reader threads pushing to queues.
- Line length cap <= 1 MB (1,048,576 bytes) and total log cap <= 50 MB per process.
- Per-request timeout (default 20s for short commands; movetime + 1000ms in games).
- On timeout/hang: send 'stop', wait 500ms, then kill process tree via 'taskkill /F /T /PID'.
- Record spawned PIDs in run/pids.txt and only kill PIDs spawned by safe_proc.
- Strip HOMEI_THREADS from child environment; send Threads=1 and Hash=64 before each game.
- Resource controls: max workers <= min(4, cpu_count // 2), free disk >= 5 GB,
  .heavy.lock (PID + timestamp, stale after 6 hours or dead PID), and progress.hb every 30s.
"""

from __future__ import annotations

import base64
import ctypes
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import time
from typing import Any, Callable, Dict, List, Optional, Set, Tuple

HOMEI_ROOT = Path(__file__).resolve().parents[1]
RUN_DIR = HOMEI_ROOT / "run"
PIDS_FILE = RUN_DIR / "pids.txt"
HEAVY_LOCK_FILE = HOMEI_ROOT / ".heavy.lock"
HEARTBEAT_FILE = HOMEI_ROOT / "progress.hb"

MAX_LINE_BYTES = 1 * 1024 * 1024       # 1 MB per line
MAX_LOG_BYTES = 50 * 1024 * 1024       # 50 MB total output per process
SHORT_CMD_TIMEOUT = 20.0               # 20s for short commands
STOP_WAIT_SEC = 0.5                    # 500ms wait after 'stop' before taskkill
MIN_FREE_DISK_BYTES = 5 * (1024 ** 3)  # 5 GB minimum free disk
LOCK_STALE_SEC = 6 * 3600              # 6 hours

_PID_LOCK = threading.Lock()
_OWNED_PIDS: Set[int] = set()


def max_safe_workers() -> int:
    """Return max concurrent game workers: <= min(4, cpu_count // 2)."""
    cpus = os.cpu_count() or 2
    return max(1, min(4, cpus // 2))


def check_free_disk(path: Path = HOMEI_ROOT, min_bytes: int = MIN_FREE_DISK_BYTES) -> int:
    """Verify at least min_bytes (default 5 GB) of free disk space is available."""
    usage = shutil.disk_usage(str(path))
    if usage.free < min_bytes:
        free_gb = usage.free / (1024 ** 3)
        req_gb = min_bytes / (1024 ** 3)
        raise RuntimeError(f"Insufficient disk space on {path}: {free_gb:.2f} GB free < {req_gb:.2f} GB required")
    return usage.free


def is_pid_alive(pid: int) -> bool:
    """Check if a process with the given PID is currently running."""
    if pid <= 0:
        return False
    if os.name == "nt":
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
        if not handle:
            return False
        try:
            exit_code = ctypes.c_ulong()
            if kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
                return exit_code.value == STILL_ACTIVE
            return False
        finally:
            kernel32.CloseHandle(handle)
    else:
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False


def _register_pid(pid: int) -> None:
    with _PID_LOCK:
        _OWNED_PIDS.add(int(pid))
        RUN_DIR.mkdir(parents=True, exist_ok=True)
        with PIDS_FILE.open("a", encoding="utf-8") as f:
            f.write(f"{int(pid)}\t{time.time():.3f}\t{os.getpid()}\n")
            f.flush()


def is_owned_pid(pid: int) -> bool:
    """Return True only if pid was registered by safe_proc."""
    pid = int(pid)
    with _PID_LOCK:
        if pid in _OWNED_PIDS:
            return True
        if not PIDS_FILE.is_file():
            return False
        try:
            for line in PIDS_FILE.read_text(encoding="utf-8", errors="ignore").splitlines():
                parts = line.strip().split("\t")
                if parts and parts[0].isdigit() and int(parts[0]) == pid:
                    _OWNED_PIDS.add(pid)
                    return True
        except OSError:
            pass
    return False


def kill_owned_pid_tree(pid: int) -> bool:
    """Kill process tree via taskkill /F /T /PID only if pid is owned by safe_proc."""
    pid = int(pid)
    if not is_owned_pid(pid):
        raise PermissionError(f"Refusing to kill PID {pid}: not registered in run/pids.txt")
    if os.name == "nt":
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            subprocess.run(
                ["taskkill", "/F", "/T", "/PID", str(pid)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=10.0,
                creationflags=flags,
                check=False,
            )
            return True
        except Exception:
            return False
    else:
        try:
            os.kill(pid, 9)
            return True
        except OSError:
            return False


class HeavyLock:
    """File lock (.heavy.lock) ensuring only one heavy task runs at a time."""

    def __init__(self, task_name: str, lock_file: Path = HEAVY_LOCK_FILE):
        self.task_name = task_name
        self.lock_file = lock_file
        self.acquired = False

    def acquire(self) -> None:
        check_free_disk()
        now = time.time()
        if self.lock_file.is_file():
            try:
                data = json.loads(self.lock_file.read_text(encoding="utf-8"))
                old_pid = int(data.get("pid", -1))
                old_ts = float(data.get("timestamp", 0.0))
                old_task = data.get("task", "unknown")
                age = now - old_ts
                if old_pid != os.getpid() and is_pid_alive(old_pid) and age < LOCK_STALE_SEC:
                    raise RuntimeError(
                        f"Heavy lock already held by active PID {old_pid} "
                        f"(task={old_task!r}, age={age:.1f}s)"
                    )
            except (ValueError, KeyError, json.JSONDecodeError, OSError):
                pass

        payload = {
            "pid": os.getpid(),
            "timestamp": now,
            "iso": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "task": self.task_name,
        }
        self.lock_file.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        self.acquired = True

    def release(self) -> None:
        if not self.acquired:
            return
        try:
            if self.lock_file.is_file():
                data = json.loads(self.lock_file.read_text(encoding="utf-8"))
                if int(data.get("pid", -1)) == os.getpid():
                    self.lock_file.unlink(missing_ok=True)
        except Exception:
            pass
        self.acquired = False

    def __enter__(self) -> "HeavyLock":
        self.acquire()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.release()


class HeartbeatMonitor:
    """Writes progress.hb every 30s and monitors progress for 5-minute stalls."""

    def __init__(
        self,
        stage: str,
        hb_file: Path = HEARTBEAT_FILE,
        interval_sec: float = 30.0,
        stall_timeout_sec: float = 300.0,
        on_stall: Optional[Callable[[str], None]] = None,
    ):
        self.stage = stage
        self.hb_file = hb_file
        self.interval_sec = interval_sec
        self.stall_timeout_sec = stall_timeout_sec
        self.on_stall = on_stall
        self._lock = threading.Lock()
        self._last_progress_ts = time.time()
        self._detail = "starting"
        self._completed = 0
        self._total = 0
        self._stop_evt = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self.stalled = False

    def update(self, detail: str = "", completed: Optional[int] = None, total: Optional[int] = None) -> None:
        with self._lock:
            self._last_progress_ts = time.time()
            if detail:
                self._detail = detail
            if completed is not None:
                self._completed = completed
            if total is not None:
                self._total = total
        self._write_hb()

    def _write_hb(self) -> None:
        with self._lock:
            now = time.time()
            payload = {
                "pid": os.getpid(),
                "timestamp": now,
                "iso": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "stage": self.stage,
                "detail": self._detail,
                "completed": self._completed,
                "total": self._total,
                "seconds_since_progress": round(now - self._last_progress_ts, 1),
            }
        try:
            tmp = self.hb_file.with_suffix(".hb.tmp")
            tmp.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")
            tmp.replace(self.hb_file)
        except OSError:
            pass

    def _loop(self) -> None:
        while not self._stop_evt.wait(self.interval_sec):
            self._write_hb()
            with self._lock:
                idle = time.time() - self._last_progress_ts
                detail = self._detail
            if idle >= self.stall_timeout_sec:
                self.stalled = True
                if self.on_stall is not None:
                    try:
                        self.on_stall(f"No progress for {idle:.1f}s in stage={self.stage} ({detail})")
                    except Exception:
                        pass

    def start(self) -> "HeartbeatMonitor":
        self._write_hb()
        self._thread = threading.Thread(target=self._loop, daemon=True)
        self._thread.start()
        return self

    def stop(self) -> None:
        self._stop_evt.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        self._write_hb()

    def __enter__(self) -> "HeartbeatMonitor":
        return self.start()

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.stop()


class LogLimitExceededError(RuntimeError):
    """Raised when a child process exceeds MAX_LOG_BYTES (50 MB)."""


class SafeProcess:
    """Safe subprocess wrapper with separate process group, reader queues, and caps."""

    def __init__(
        self,
        args: List[str],
        cwd: Optional[Path] = None,
        extra_env: Optional[Dict[str, str]] = None,
        max_line_bytes: int = MAX_LINE_BYTES,
        max_log_bytes: int = MAX_LOG_BYTES,
    ):
        self.args = [str(a) for a in args]
        self.cwd = str(cwd) if cwd else None
        self.extra_env = dict(extra_env) if extra_env else {}
        self.max_line_bytes = max_line_bytes
        self.max_log_bytes = max_log_bytes

        self.proc: Optional[subprocess.Popen] = None
        self.pid: Optional[int] = None
        self.stdout_queue: "queue.Queue[Optional[Tuple[str, bytes, bool]]]" = queue.Queue()
        self.stderr_queue: "queue.Queue[Optional[Tuple[str, bytes, bool]]]" = queue.Queue()

        self._bytes_lock = threading.Lock()
        self.total_bytes_read = 0
        self.log_limit_exceeded = False
        self.line_truncated_count = 0

        self._stdout_thread: Optional[threading.Thread] = None
        self._stderr_thread: Optional[threading.Thread] = None
        self._closed = False

    def start(self) -> None:
        env = os.environ.copy()
        env.pop("HOMEI_THREADS", None)
        for k, v in self.extra_env.items():
            if v is None:
                env.pop(k, None)
            else:
                env[str(k)] = str(v)
        # Always ensure HOMEI_THREADS is removed even if present in extra_env
        env.pop("HOMEI_THREADS", None)

        creationflags = 0
        if os.name == "nt":
            creationflags |= getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
            creationflags |= getattr(subprocess, "CREATE_NO_WINDOW", 0)

        self.proc = subprocess.Popen(
            self.args,
            cwd=self.cwd,
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=-1,
            creationflags=creationflags,
        )
        self.pid = int(self.proc.pid)
        _register_pid(self.pid)

        self._stdout_thread = threading.Thread(
            target=self._reader_loop,
            args=(self.proc.stdout, self.stdout_queue, "stdout"),
            daemon=True,
        )
        self._stderr_thread = threading.Thread(
            target=self._reader_loop,
            args=(self.proc.stderr, self.stderr_queue, "stderr"),
            daemon=True,
        )
        self._stdout_thread.start()
        self._stderr_thread.start()

    def _reader_loop(
        self,
        stream,
        out_q: "queue.Queue[Optional[Tuple[str, bytes, bool]]]",
        stream_name: str,
    ) -> None:
        try:
            while True:
                chunk = stream.readline(self.max_line_bytes)
                if not chunk:
                    break

                was_truncated = False
                if len(chunk) >= self.max_line_bytes and not chunk.endswith(b"\n"):
                    was_truncated = True
                    self.line_truncated_count += 1

                with self._bytes_lock:
                    self.total_bytes_read += len(chunk)
                    exceeded = self.total_bytes_read > self.max_log_bytes
                    if exceeded and not self.log_limit_exceeded:
                        self.log_limit_exceeded = True
                        out_q.put(("__LOG_LIMIT__", b"", True))

                if not exceeded:
                    out_q.put(("line", chunk, was_truncated))
                else:
                    # Drain pipe without buffering in memory, or break once process is killed
                    break
        except Exception:
            pass
        finally:
            out_q.put(None)

    def send_line(self, line: str) -> None:
        if self.proc is None or self.proc.stdin is None:
            raise RuntimeError("Process stdin is not available")
        data = (line + "\n").encode("utf-8")
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def read_stdout_line(self, timeout: float = SHORT_CMD_TIMEOUT) -> str:
        """Read one line from stdout queue within timeout, enforcing line & total log caps."""
        if self.log_limit_exceeded:
            self.stop_and_kill()
            raise LogLimitExceededError(
                f"Process PID {self.pid} exceeded max log limit ({self.max_log_bytes} bytes)"
            )

        try:
            item = self.stdout_queue.get(timeout=timeout)
        except queue.Empty:
            self.stop_and_kill()
            raise TimeoutError(f"Timed out after {timeout:.3f}s waiting for stdout from PID {self.pid}")

        if item is None:
            if self.log_limit_exceeded:
                self.stop_and_kill()
                raise LogLimitExceededError(
                    f"Process PID {self.pid} exceeded max log limit ({self.max_log_bytes} bytes)"
                )
            raise EOFError(f"Process PID {self.pid} stdout closed unexpectedly")

        kind, raw_bytes, _ = item
        if kind == "__LOG_LIMIT__" or self.log_limit_exceeded:
            self.stop_and_kill()
            raise LogLimitExceededError(
                f"Process PID {self.pid} exceeded max log limit ({self.max_log_bytes} bytes)"
            )

        return raw_bytes.decode("utf-8", errors="replace").strip()

    def drain_stderr(self, max_lines: int = 200) -> List[str]:
        lines: List[str] = []
        while len(lines) < max_lines:
            try:
                item = self.stderr_queue.get_nowait()
            except queue.Empty:
                break
            if item is None:
                break
            kind, raw_bytes, _ = item
            if kind == "line":
                lines.append(raw_bytes.decode("utf-8", errors="replace").strip())
        return lines

    def stop_and_kill(self, req_id: Optional[int] = None, wait_sec: float = STOP_WAIT_SEC) -> None:
        """Send 'stop', wait 500ms, then kill entire process tree via taskkill /F /T /PID."""
        if self.proc is None or self.pid is None:
            return
        if self.proc.poll() is None:
            try:
                if req_id is not None:
                    self.send_line(f"stop id {req_id}")
                self.send_line("stop")
            except Exception:
                pass
            try:
                self.proc.wait(timeout=wait_sec)
            except subprocess.TimeoutExpired:
                pass
            except Exception:
                pass

        # Always invoke kill_owned_pid_tree to terminate any child processes in the tree
        if is_owned_pid(self.pid):
            kill_owned_pid_tree(self.pid)

        try:
            self.proc.wait(timeout=1.0)
        except Exception:
            pass

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self.proc is None:
            return
        if self.proc.poll() is None:
            try:
                self.send_line("quit")
                self.proc.wait(timeout=0.5)
            except Exception:
                pass
        if self.proc.poll() is None and self.pid is not None:
            self.stop_and_kill()
        elif self.pid is not None and is_owned_pid(self.pid):
            # Ensure no lingering children remain
            kill_owned_pid_tree(self.pid)
        try:
            if self.proc.stdin:
                self.proc.stdin.close()
        except Exception:
            pass
        self.proc = None

    def __enter__(self) -> "SafeProcess":
        self.start()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()


def b64url_encode(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def b64url_decode(token: str) -> bytes:
    cleaned = token.strip()
    if not cleaned:
        raise ValueError("Empty base64url token")
    pad = "=" * ((-len(cleaned)) % 4)
    return base64.urlsafe_b64decode(cleaned + pad)


def encode_state64(state_dict: dict) -> str:
    raw = json.dumps(state_dict, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return b64url_encode(raw)


def decode_state64(token: str) -> dict:
    return json.loads(b64url_decode(token).decode("utf-8"))


@dataclass
class SafeSearchOutput:
    req_id: int
    status: str  # "ok" | "timeout" | "engine_error"
    bestmove_raw: str = ""
    bestmove: Optional[dict] = None
    infos: List[dict] = field(default_factory=list)
    strings: List[str] = field(default_factory=list)
    elapsed_ms: float = 0.0


class SafeEngineClient:
    """CLP-EP v1 client built strictly on top of SafeProcess."""

    def __init__(
        self,
        executable: Path,
        extra_env: Optional[Dict[str, str]] = None,
        cmd_timeout: float = SHORT_CMD_TIMEOUT,
    ):
        self.executable = Path(executable)
        self.extra_env = dict(extra_env) if extra_env else {}
        self.cmd_timeout = cmd_timeout
        self.sproc: Optional[SafeProcess] = None
        self.handshake_lines: List[str] = []
        self.engine_id_name: str = ""
        self._next_id = 0

    def start(self, do_handshake: bool = True, init_game_options: bool = True) -> None:
        self.sproc = SafeProcess([str(self.executable)], extra_env=self.extra_env)
        self.sproc.start()
        if do_handshake:
            self.handshake()
            if init_game_options:
                self.prepare_new_game()

    def close(self) -> None:
        if self.sproc is not None:
            self.sproc.close()
            self.sproc = None

    def __enter__(self) -> "SafeEngineClient":
        self.start(do_handshake=True, init_game_options=True)
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def send_line(self, line: str) -> None:
        assert self.sproc is not None
        self.sproc.send_line(line)

    def read_line(self, timeout: Optional[float] = None) -> str:
        assert self.sproc is not None
        t = self.cmd_timeout if timeout is None else timeout
        return self.sproc.read_stdout_line(timeout=t)

    def handshake(self) -> List[str]:
        self.send_line("clp 1")
        lines: List[str] = []
        while True:
            line = self.read_line(timeout=self.cmd_timeout)
            lines.append(line)
            if line.startswith("id name "):
                self.engine_id_name = line[len("id name "):].strip()
            if line == "clpok":
                break
        self.handshake_lines = lines
        self.send_line("isready")
        ready = self.read_line(timeout=self.cmd_timeout)
        if ready != "readyok":
            raise RuntimeError(f"Expected readyok, got {ready!r}")
        return lines

    def set_option(self, name: str, value: Any) -> str:
        self.send_line(f"setoption name {name} value {value}")
        resp = self.read_line(timeout=self.cmd_timeout)
        expected = f"optionok name {name} value {value}"
        if resp != expected:
            raise RuntimeError(f"Expected {expected!r}, got {resp!r}")
        return resp

    def newgame(self) -> str:
        self.send_line("newgame")
        resp = self.read_line(timeout=self.cmd_timeout)
        if resp != "newgameok":
            raise RuntimeError(f"Expected 'newgameok', got {resp!r}")
        return resp

    def prepare_new_game(self, hash_mb: int = 64, threads: int = 1) -> None:
        """Always send setoption Threads=1 and Hash=64 before each game."""
        self.set_option("Threads", threads)
        self.set_option("Hash", hash_mb)
        lines = set(self.handshake_lines)
        if any(l.startswith("option name MultiPV ") for l in lines):
            self.set_option("MultiPV", 1)
        if any(l.startswith("option name Depth ") for l in lines):
            self.set_option("Depth", 32)
        if any(l.startswith("option name Nodes ") for l in lines):
            self.set_option("Nodes", 50000000)

    def set_position_state(self, state_dict: dict) -> str:
        b64 = encode_state64(state_dict)
        self.send_line(f"position state64 {b64}")
        resp = self.read_line(timeout=self.cmd_timeout)
        if not resp.startswith("positionok "):
            raise RuntimeError(f"position state64 failed: {resp!r}")
        return resp

    def set_position_lppn(self, lppn_str: str) -> str:
        self.send_line(f"position lppn {lppn_str}")
        resp = self.read_line(timeout=self.cmd_timeout)
        if not resp.startswith("positionok "):
            raise RuntimeError(f"position lppn failed: {resp!r}")
        return resp

    def search_movetime(
        self,
        state_dict: dict,
        movetime_ms: int,
        grace_ms: int = 1000,
    ) -> SafeSearchOutput:
        """Search with strict deadline = movetime_ms + grace_ms.

        On deadline expiration, sends stop, waits 500ms, then kills process tree.
        """
        assert self.sproc is not None
        self.set_position_state(state_dict)
        self._next_id += 1
        req_id = self._next_id
        t0 = time.perf_counter()
        deadline = t0 + (movetime_ms + grace_ms) / 1000.0
        self.send_line(f"go id {req_id} movetime {movetime_ms}")

        infos: List[dict] = []
        strings: List[str] = []

        while True:
            rem = deadline - time.perf_counter()
            if rem <= 0.0:
                self.sproc.stop_and_kill(req_id=req_id, wait_sec=STOP_WAIT_SEC)
                return SafeSearchOutput(
                    req_id=req_id,
                    status="timeout",
                    infos=infos,
                    strings=strings,
                    elapsed_ms=(time.perf_counter() - t0) * 1000.0,
                )
            try:
                line = self.read_line(timeout=rem)
            except TimeoutError:
                self.sproc.stop_and_kill(req_id=req_id, wait_sec=STOP_WAIT_SEC)
                return SafeSearchOutput(
                    req_id=req_id,
                    status="timeout",
                    infos=infos,
                    strings=strings,
                    elapsed_ms=(time.perf_counter() - t0) * 1000.0,
                )
            except (EOFError, OSError, RuntimeError):
                self.sproc.stop_and_kill(req_id=req_id, wait_sec=STOP_WAIT_SEC)
                return SafeSearchOutput(
                    req_id=req_id,
                    status="engine_error",
                    infos=infos,
                    strings=strings,
                    elapsed_ms=(time.perf_counter() - t0) * 1000.0,
                )

            if line.startswith(f"info id {req_id} info64 "):
                try:
                    token = line.split(" ", 4)[4]
                    infos.append(json.loads(b64url_decode(token).decode("utf-8")))
                except Exception:
                    pass
            elif line.startswith(f"info id {req_id} string64 "):
                try:
                    token = line.split(" ", 4)[4]
                    msg_obj = json.loads(b64url_decode(token).decode("utf-8"))
                    strings.append(msg_obj.get("message", ""))
                except Exception:
                    pass
            elif line.startswith(f"bestmove id {req_id} "):
                elapsed_ms = (time.perf_counter() - t0) * 1000.0
                parts = line.split(" ")
                bestmove_obj = None
                if len(parts) == 5 and parts[3] == "move64":
                    try:
                        bestmove_obj = json.loads(b64url_decode(parts[4]).decode("utf-8"))
                    except Exception:
                        return SafeSearchOutput(
                            req_id=req_id,
                            status="engine_error",
                            bestmove_raw=line,
                            infos=infos,
                            strings=strings,
                            elapsed_ms=elapsed_ms,
                        )
                return SafeSearchOutput(
                    req_id=req_id,
                    status="ok",
                    bestmove_raw=line,
                    bestmove=bestmove_obj,
                    infos=infos,
                    strings=strings,
                    elapsed_ms=elapsed_ms,
                )
            elif line.startswith("error "):
                return SafeSearchOutput(
                    req_id=req_id,
                    status="engine_error",
                    bestmove_raw=line,
                    infos=infos,
                    strings=strings,
                    elapsed_ms=(time.perf_counter() - t0) * 1000.0,
                )

    def search_fixed_depth(
        self,
        lppn_or_state: Any,
        depth: int,
        multipv: int = 1,
        movetime_ms: int = 15000,
    ) -> SafeSearchOutput:
        """Run a fixed-depth search using Option Depth=<depth> and movetime."""
        if isinstance(lppn_or_state, str):
            self.set_position_lppn(lppn_or_state)
        else:
            self.set_position_state(lppn_or_state)
        self.set_option("Depth", depth)
        self.set_option("MultiPV", multipv)
        self._next_id += 1
        req_id = self._next_id
        t0 = time.perf_counter()
        deadline = t0 + (movetime_ms + 1000) / 1000.0
        self.send_line(f"go id {req_id} movetime {movetime_ms}")

        infos: List[dict] = []
        strings: List[str] = []
        assert self.sproc is not None
        while True:
            rem = deadline - time.perf_counter()
            if rem <= 0.0:
                self.sproc.stop_and_kill(req_id=req_id, wait_sec=STOP_WAIT_SEC)
                return SafeSearchOutput(req_id=req_id, status="timeout", infos=infos, strings=strings)
            line = self.read_line(timeout=rem)
            if line.startswith(f"info id {req_id} info64 "):
                token = line.split(" ", 4)[4]
                infos.append(json.loads(b64url_decode(token).decode("utf-8")))
            elif line.startswith(f"info id {req_id} string64 "):
                token = line.split(" ", 4)[4]
                strings.append(json.loads(b64url_decode(token).decode("utf-8")).get("message", ""))
            elif line.startswith(f"bestmove id {req_id} "):
                elapsed_ms = (time.perf_counter() - t0) * 1000.0
                parts = line.split(" ")
                bestmove_obj = None
                if len(parts) == 5 and parts[3] == "move64":
                    bestmove_obj = json.loads(b64url_decode(parts[4]).decode("utf-8"))
                return SafeSearchOutput(
                    req_id=req_id,
                    status="ok",
                    bestmove_raw=line,
                    bestmove=bestmove_obj,
                    infos=infos,
                    strings=strings,
                    elapsed_ms=elapsed_ms,
                )
            elif line.startswith("error "):
                return SafeSearchOutput(req_id=req_id, status="engine_error", bestmove_raw=line)

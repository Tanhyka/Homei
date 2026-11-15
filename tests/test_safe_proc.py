"""Unit tests for operational safety wrapper tools/safe_proc.py."""

from __future__ import annotations

import os
from pathlib import Path
import sys
import time
import pytest

HOMEI_ROOT = Path(__file__).resolve().parents[1]
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))

from tools.safe_proc import (
    HeavyLock,
    HeartbeatMonitor,
    LogLimitExceededError,
    SafeProcess,
    is_owned_pid,
    is_pid_alive,
    kill_owned_pid_tree,
)


def test_unresponsive_process_timeout_and_kill():
    """Case 1: Fake process never answers -> safe_proc times out on schedule and kills it."""
    code = "import time\nwhile True:\n    time.sleep(1.0)\n"
    sproc = SafeProcess([sys.executable, "-u", "-c", code])
    sproc.start()
    pid = sproc.pid
    assert pid is not None
    assert is_owned_pid(pid)
    assert is_pid_alive(pid)

    t0 = time.perf_counter()
    with pytest.raises(TimeoutError):
        sproc.read_stdout_line(timeout=0.4)
    elapsed = time.perf_counter() - t0

    # Timeout (0.4s) + stop wait (0.5s) + taskkill overhead
    assert 0.35 <= elapsed <= 5.0
    time.sleep(0.1)
    assert not is_pid_alive(pid)
    sproc.close()


def test_100mb_flood_then_hang():
    """Case 2: Fake process prints 100 MB then hangs -> safe_proc caps line (<=1MB) and log (<=50MB) and kills."""
    # Prints 100 chunks of 1 MB without newline or with newline, totaling 100 MB, then hangs
    code = (
        "import sys, time\n"
        "chunk = b'X' * (1024 * 1024)\n"
        "try:\n"
        "    for _ in range(100):\n"
        "        sys.stdout.buffer.write(chunk)\n"
        "        sys.stdout.buffer.flush()\n"
        "except Exception:\n"
        "    pass\n"
        "while True:\n"
        "    time.sleep(1.0)\n"
    )
    sproc = SafeProcess([sys.executable, "-u", "-c", code])
    sproc.start()
    pid = sproc.pid
    assert pid is not None

    lines_read = 0
    max_observed_line = 0
    hit_limit = False

    try:
        for _ in range(200):
            line = sproc.read_stdout_line(timeout=3.0)
            lines_read += 1
            max_observed_line = max(max_observed_line, len(line))
            assert len(line) <= 1024 * 1024
    except (LogLimitExceededError, TimeoutError, EOFError):
        hit_limit = True

    assert hit_limit
    assert max_observed_line <= 1024 * 1024
    assert sproc.log_limit_exceeded or sproc.total_bytes_read <= 52 * 1024 * 1024
    sproc.stop_and_kill()
    time.sleep(0.1)
    assert not is_pid_alive(pid)
    sproc.close()


def test_ignore_stop_and_spawn_child_tree_killed():
    """Case 3: Fake process spawns a child process and ignores 'stop' -> entire tree is killed."""
    code = (
        "import subprocess, sys, time\n"
        "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(120)'])\n"
        "print(f'CHILD_PID {child.pid}', flush=True)\n"
        "while True:\n"
        "    try:\n"
        "        line = sys.stdin.readline()\n"
        "        # Intentionally ignore 'stop' and keep running\n"
        "    except Exception:\n"
        "        pass\n"
        "    time.sleep(0.2)\n"
    )
    sproc = SafeProcess([sys.executable, "-u", "-c", code])
    sproc.start()
    parent_pid = sproc.pid
    assert parent_pid is not None

    first_line = sproc.read_stdout_line(timeout=5.0)
    assert first_line.startswith("CHILD_PID ")
    child_pid = int(first_line.split()[1])

    assert is_pid_alive(parent_pid)
    assert is_pid_alive(child_pid)

    # Now wait for a second line that never arrives -> triggers stop + 500ms wait + taskkill /F /T /PID
    with pytest.raises(TimeoutError):
        sproc.read_stdout_line(timeout=0.3)

    time.sleep(0.2)
    assert not is_pid_alive(parent_pid), f"Parent PID {parent_pid} should be dead"
    assert not is_pid_alive(child_pid), f"Child PID {child_pid} should be dead after tree kill"
    sproc.close()


def test_strip_homei_threads_and_pid_protection(monkeypatch, tmp_path):
    """Verify HOMEI_THREADS is stripped from child environment and non-owned PIDs cannot be killed."""
    monkeypatch.setenv("HOMEI_THREADS", "16")
    code = "import os\nprint('VAL=' + str(os.environ.get('HOMEI_THREADS')))\n"
    with SafeProcess([sys.executable, "-u", "-c", code]) as sproc:
        out = sproc.read_stdout_line(timeout=5.0)
        assert out == "VAL=None"

    with pytest.raises(PermissionError):
        kill_owned_pid_tree(99999999)

    lock_path = tmp_path / ".heavy.lock"
    with HeavyLock("unit_test", lock_file=lock_path):
        assert lock_path.is_file()
    assert not lock_path.is_file()

    hb_path = tmp_path / "progress.hb"
    with HeartbeatMonitor("unit_test", hb_file=hb_path, interval_sec=0.1) as hb:
        hb.update("step 1", completed=1, total=5)
        time.sleep(0.15)
        assert hb_path.is_file()

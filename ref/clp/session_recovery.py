"""Session recovery and autosave management for CLP_GUI_TRAINING.

Protects against unexpected exits, power outages, and crashes by atomically
persisting active game positions, teaching notes, evaluations, and window setups.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import tempfile
import time
from typing import Any, Dict, Optional

RECOVERY_DIR = Path.home() / ".clp_gui_training"
RECOVERY_FILE = RECOVERY_DIR / "recovery_session.json"
SCHEMA = "clp.recovery-session/1"


class SessionRecovery:
    """Manages periodic auto-saves and crash recovery files."""

    @staticmethod
    def get_recovery_file() -> Path:
        RECOVERY_DIR.mkdir(parents=True, exist_ok=True)
        return RECOVERY_FILE

    @classmethod
    def save_snapshot(cls, payload: Dict[str, Any], clean_exit: bool = False) -> bool:
        """Atomically saves session snapshot to prevent file corruption during sudden power cut."""
        try:
            target = cls.get_recovery_file()
            data = dict(payload)
            data["schema"] = SCHEMA
            data["timestamp"] = time.time()
            data["clean_exit"] = bool(clean_exit)

            content = json.dumps(data, indent=2, ensure_ascii=False).encode("utf-8")

            # Atomic write via temp file in same directory
            temp_fd, temp_path = tempfile.mkstemp(dir=str(target.parent), prefix="recovery_", suffix=".tmp")
            with os.fdopen(temp_fd, "wb") as f:
                f.write(content)
                f.flush()
                os.fsync(f.fileno())

            # Atomic replace
            os.replace(temp_path, str(target))
            return True
        except Exception:
            return False

    @classmethod
    def load_snapshot(cls) -> Optional[Dict[str, Any]]:
        """Loads recovery snapshot if available and valid."""
        try:
            target = cls.get_recovery_file()
            if not target.is_file():
                return None
            raw = target.read_bytes()
            if not raw:
                return None
            data = json.loads(raw.decode("utf-8"))
            if not isinstance(data, dict) or data.get("schema") != SCHEMA:
                return None
            return data
        except Exception:
            return None

    @classmethod
    def has_interrupted_session(cls) -> bool:
        """Checks if a valid recovery snapshot exists with unfinished work or unexpected exit."""
        data = cls.load_snapshot()
        if not data:
            return False

        # If cleanly exited and game was at start position with no notes, no restore needed
        clean_exit = data.get("clean_exit", False)
        comment_session = data.get("comment_session", {})
        moves = comment_session.get("moves", [])
        draft_note = data.get("draft_note", "").strip()
        teaching_decisions = data.get("teaching_decisions", [])

        has_work = len(moves) > 0 or len(draft_note) > 0 or len(teaching_decisions) > 0

        # Interrupted if power cut/crash (clean_exit is False), or has unsaved game progress
        return (not clean_exit and has_work) or (has_work)

    @classmethod
    def mark_clean_exit(cls) -> bool:
        """Marks current session as cleanly exited."""
        data = cls.load_snapshot()
        if data:
            return cls.save_snapshot(data, clean_exit=True)
        return True

    @classmethod
    def clear(cls) -> bool:
        """Clears the recovery snapshot."""
        try:
            target = cls.get_recovery_file()
            if target.is_file():
                target.unlink()
            return True
        except Exception:
            return False

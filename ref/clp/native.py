"""Native C++ rules service bridge (cpp-CLP / clp_rules_service.dll) for fast training & validation."""

from __future__ import annotations

import ctypes
import json
import os
import threading
from pathlib import Path


class CoreUnavailable(RuntimeError):
    """The authoritative C++ rules DLL is not available."""


def _candidates():
    """Explicit override first; otherwise check cpp-CLP build directories in ColongphuongGUI_train."""
    configured = os.environ.get("CLP_CORE_LIBRARY", "").strip().strip('"')
    if configured:
        yield Path(configured).expanduser()
        return
    project = Path(__file__).resolve().parents[1]
    workspace = project.parent
    roots = [
        project / "cpp-CLP",
        project / "bin",
        project / "external" / "cpp-CLP",
        workspace / "code4" / "CLP_GUI_TRAINING" / "external" / "cpp-CLP",
        workspace / "Library" / "cpp-CLP",
        workspace / "CLP_engine" / "Ryuugin",
    ]
    children = (
        "",
        "build",
        "build/Release",
        "build/Debug",
        "build/windows-release/Release",
        "build/windows-debug/Debug",
        "build/msvc2022-x64/Release",
        "build/step3",
    )
    filenames = ("clp_rules_service.dll", "ryuugin_rules.dll")
    for root in roots:
        for child in children:
            for filename in filenames:
                yield root / child / filename


class NativeCore:
    """Authoritative C++ wrapper calling clp_core_json in clp_rules_service.dll."""

    def __init__(self, path: Path | str):
        self.path = Path(path)
        self.library = ctypes.CDLL(str(self.path))
        self.function = self.library.clp_core_json
        self.function.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t]
        self.function.restype = ctypes.c_size_t
        self.buffer = ctypes.create_string_buffer(65536)
        self.lock = threading.RLock()
        self.metadata = self.call("metadata")
        if self.metadata.get("api") != "ryuugin.rules/1":
            raise RuntimeError("DLL provides incompatible API (expected ryuugin.rules/1)")
        self.available = True
        self.problem = ""

    def call(self, command: str, **fields):
        wire = json.dumps(
            {"command": command, **fields},
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
        if len(wire) > 1048576:
            raise ValueError("Rules request exceeds CLP payload limit; history must not be truncated")
        with self.lock:
            needed = self.function(wire, self.buffer, len(self.buffer))
            if needed > 1048577 or needed == 0:
                raise ValueError("Invalid rule-service response size")
            if needed > len(self.buffer):
                self.buffer = ctypes.create_string_buffer(needed)
                if self.function(wire, self.buffer, len(self.buffer)) != needed:
                    raise ValueError("Unstable rule-service response")
            result = json.loads(self.buffer.value.decode("utf-8"))
        if isinstance(result, dict) and "error" in result:
            raise ValueError(result["error"])
        return result


class UnavailableCore:
    """Fallback descriptor when the optional C++ DLL is not loaded."""

    available = False
    path = None
    metadata = {"api": "unavailable/1"}

    def __init__(self, reason: str):
        self.problem = reason

    def call(self, command: str, **fields):
        raise CoreUnavailable(
            f"Native C++ cpp-CLP DLL not available ({command}). {self.problem}"
        )


_core = None
_lock = threading.Lock()


def native_core(reload: bool = False) -> NativeCore | UnavailableCore:
    global _core
    with _lock:
        if _core is None or reload:
            _core = None
            tried = []
            for candidate in _candidates():
                if not candidate.is_file():
                    tried.append(str(candidate))
                    continue
                try:
                    _core = NativeCore(candidate)
                    break
                except (OSError, AttributeError, RuntimeError, ValueError) as error:
                    tried.append(f"{candidate} — {type(error).__name__}: {error}")
            if _core is None:
                override = os.environ.get("CLP_CORE_LIBRARY")
                detail = (
                    "CLP_CORE_LIBRARY=" + override
                    if override
                    else "No compatible clp_rules_service.dll found in cpp-CLP/build."
                )
                if tried and override:
                    detail += "\n" + tried[0]
                _core = UnavailableCore(detail)
    return _core

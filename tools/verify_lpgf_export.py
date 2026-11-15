"""Verify 100% of exported EvE LPGF v2.0 files using ref.clp.record_io."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import List

HOMEI_ROOT = Path(__file__).resolve().parents[1]
if str(HOMEI_ROOT) not in sys.path:
    sys.path.insert(0, str(HOMEI_ROOT))

from ref.clp import core_bridge
from ref.clp import record_io

DEFAULT_EVE_DIR = Path(r"E:\code5\CLP_dataset\LPGF\EvE\Homei_v3_2_vs_v3")


def verify_directory(root_dir: Path) -> dict:
    files = sorted(root_dir.rglob("*.lpgf"))
    total = len(files)
    passed = 0
    errors: List[dict] = []

    for fpath in files:
        err_list: List[str] = []
        rec = record_io.ImportedRecord()
        if not record_io.RecordIO.read(str(fpath), rec, err_list):
            errors.append({"file": str(fpath), "stage": "read", "error": err_list})
            continue
        if rec.kind != record_io.RecordKind.Lpgf20:
            errors.append({"file": str(fpath), "stage": "kind", "error": f"Expected Lpgf20, got {rec.kind}"})
            continue
        bridge = core_bridge.CoreBridge()
        app_plies, tot_tokens, err_str = record_io.RecordIO.load_txt_moves_into_bridge(bridge, rec)
        if err_str is not None or app_plies != tot_tokens:
            errors.append(
                {
                    "file": str(fpath),
                    "stage": "replay",
                    "applied": app_plies,
                    "tokens": tot_tokens,
                    "error": err_str,
                }
            )
            continue
        passed += 1

    summary = {
        "root_dir": str(root_dir),
        "total_files": total,
        "passed_files": passed,
        "failed_files": len(errors),
        "errors": errors[:20],
    }
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    if errors or total == 0:
        raise SystemExit(1)
    return summary


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify exported LPGF v2.0 files.")
    parser.add_argument("--dir", type=Path, default=DEFAULT_EVE_DIR)
    args = parser.parse_args()
    verify_directory(args.dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

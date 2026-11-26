"""3-Round Clean Selection Tournament for Step 7 Self-Play (`E:\\all-code\\Code1\\CLP_dataset\\LPGF\\selfplay`).

Structure (100% matching user specification):
  - Round 1 (`Arena 1.0`): 7 Homei engines (v3-2, v4, v4-1, v4-2, v4-3, v4-4, v4-5)
    round-robin (21 pairs), 150ms & 500ms -> top 4 by Bradley-Terry / score Elo advance!
  - Round 2 (`High dan Arena`): Top 4 round-robin (6 pairs), 150ms & 500ms -> top 2 advance!
  - Round 3 (Final — 3 Obstacles): Top 2 compete across:
      1) `Book`           (from book_openings.txt)
      2) `Seeding`        (from seeding_hvh.txt — real human games)
      3) `Final_Startpos` (from standard initial position)
    at both 150ms & 500ms -> crowns the official Champion!
"""

from __future__ import annotations

import argparse
import collections
import itertools
import json
import math
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, List, Tuple

HOMEI_ROOT = Path(r"E:\all-code\Code1\CLP_engine\Homei")
RELEASE_DIR = HOMEI_ROOT / "Engine release"
MATCH_PY = HOMEI_ROOT / "tools" / "match.py"
SELFPLAY_ROOT = Path(r"E:\all-code\Code1\CLP_dataset\LPGF\selfplay")
GAMES_DIR = HOMEI_ROOT / "games"

ENGINES: Dict[str, Path] = {
    "Homei_v3-2": RELEASE_DIR / "Homei_v3-2.exe",
    "Homei_v4-0": RELEASE_DIR / "Homei_v4.exe",
    "Homei_v4-1": RELEASE_DIR / "Homei_v4-1.exe",
    "Homei_v4-2": RELEASE_DIR / "Homei_v4-2.exe",
    "Homei_v4-3": RELEASE_DIR / "Homei_v4-3.exe",
    "Homei_v4-4": RELEASE_DIR / "Homei_v4-4.exe",
    "Homei_v4-5": RELEASE_DIR / "Homei_v4-5.exe",
}


def run_pair_match(
    run_name: str,
    eng_a: str,
    eng_b: str,
    games: int,
    movetime_ms: int,
    lpgf_root: Path,
    *,
    startpos: bool = True,
    openings: Path | None = None,
    workers: int = 4,
) -> Tuple[int, int, int]:
    """Run (or resume) a match via match.py and return (wins_a, draws, losses_a)."""
    lpgf_root.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable, "-u", str(MATCH_PY),
        "--run", run_name,
        "--engine-a", str(ENGINES[eng_a]),
        "--engine-b", str(ENGINES[eng_b]),
        "--name-a", eng_a,
        "--name-b", eng_b,
        "--games", str(games),
        "--movetime", str(movetime_ms),
        "--lpgf-root", str(lpgf_root),
        "--workers", str(workers),
        "--progress-interval", str(max(4, games)),
    ]
    if startpos:
        cmd.append("--startpos")
    elif openings is not None:
        cmd.extend(["--openings", str(openings)])

    t0 = time.perf_counter()
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        print(f"[ERROR] {run_name} failed rc={proc.returncode}\n{proc.stdout[-800:]}\n{proc.stderr[-800:]}", flush=True)
        raise RuntimeError(f"Match failed: {run_name}")

    # Parse exact W/D/L from games/<run_name>/results.jsonl
    rfile = GAMES_DIR / run_name / "results.jsonl"
    w = d = l = 0
    if rfile.is_file():
        for raw in rfile.read_text(encoding="utf-8").splitlines():
            if not raw.strip():
                continue
            row = json.loads(raw)
            sc = float(row.get("score_a", 0.5))
            if sc > 0.75:
                w += 1
            elif sc < 0.25:
                l += 1
            else:
                d += 1
    dt = time.perf_counter() - t0
    print(f"  [MATCH] {run_name:44s} ({movetime_ms:3d}ms, {games:2d}g): {eng_a} {w}/{d}/{l} {eng_b} ({dt:.1f}s)", flush=True)
    return w, d, l


def compute_bt_elo(participants: List[str], pair_results: List[Tuple[str, str, int, int, int]]) -> List[dict]:
    """Compute Bradley-Terry Elo (anchored at mean=2500 or v3-2=2000 if present) + raw W/D/L."""
    stats = {p: {"W": 0, "D": 0, "L": 0, "pts": 0.0, "games": 0} for p in participants}
    # Pairwise matrix: wins_ij (including 0.5*draws) and total_ij
    w_mat = collections.defaultdict(float)
    n_mat = collections.defaultdict(float)
    for a, b, w, d, l in pair_results:
        tot = w + d + l
        if tot == 0:
            continue
        pa = w + 0.5 * d
        pb = l + 0.5 * d
        stats[a]["W"] += w; stats[a]["D"] += d; stats[a]["L"] += l; stats[a]["pts"] += pa; stats[a]["games"] += tot
        stats[b]["W"] += l; stats[b]["D"] += d; stats[b]["L"] += w; stats[b]["pts"] += pb; stats[b]["games"] += tot
        w_mat[(a, b)] += pa
        w_mat[(b, a)] += pb
        n_mat[(a, b)] += tot
        n_mat[(b, a)] += tot

    # Iterative Bradley-Terry with small Laplace prior (0.25) so 100%-win or 0%-win doesn't diverge to +/-inf
    ratings = {p: 1.0 for p in participants}
    prior = 0.25
    for _ in range(300):
        new_r = {}
        for i in participants:
            num = prior
            den = prior
            for j in participants:
                if i == j:
                    continue
                nij = n_mat[(i, j)]
                if nij <= 0:
                    continue
                num += w_mat[(i, j)]
                den += nij / (ratings[i] + ratings[j])
            new_r[i] = max(1e-6, num / max(1e-9, den))
        # Normalize geometric mean
        gm = math.exp(sum(math.log(v) for v in new_r.values()) / len(new_r))
        ratings = {p: v / gm for p, v in new_r.items()}

    elos = {p: 400.0 * math.log10(ratings[p]) for p in participants}
    anchor = elos["Homei_v3-2"] - 2000.0 if "Homei_v3-2" in elos else (sum(elos.values()) / len(elos) - 2600.0)
    elos = {p: round(v - anchor, 1) for p, v in elos.items()}

    table = []
    for p in participants:
        st = stats[p]
        pct = (100.0 * st["pts"] / st["games"]) if st["games"] else 0.0
        table.append({
            "engine": p,
            "elo": elos[p],
            "W": st["W"],
            "D": st["D"],
            "L": st["L"],
            "score": round(st["pts"], 1),
            "games": st["games"],
            "pct": round(pct, 2),
        })
    table.sort(key=lambda r: (-r["elo"], -r["score"], -r["W"]))
    return table


def print_standings(title: str, table: List[dict]) -> None:
    print(f"\n==================== {title} ====================", flush=True)
    print(f"{'Rank':<5} {'Engine':<14} {'BT-Elo':>8} {'W/D/L':>12} {'Score':>10} {'Win%':>7}", flush=True)
    for idx, r in enumerate(table, 1):
        wdl = f"{r['W']}/{r['D']}/{r['L']}"
        sc = f"{r['score']}/{r['games']}"
        print(f"{idx:<5} {r['engine']:<14} {r['elo']:>8.1f} {wdl:>12} {sc:>10} {r['pct']:>6.1f}%", flush=True)
    print("=" * (42 + len(title)), flush=True)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--r1-150", type=int, default=10)
    ap.add_argument("--r1-500", type=int, default=6)
    ap.add_argument("--r2-150", type=int, default=16)
    ap.add_argument("--r2-500", type=int, default=8)
    ap.add_argument("--r3-150", type=int, default=16)
    ap.add_argument("--r3-500", type=int, default=8)
    ap.add_argument("--workers", type=int, default=4)
    args = ap.parse_args()

    for name, p in ENGINES.items():
        assert p.is_file(), f"Missing engine binary for {name}: {p}"

    book_file = SELFPLAY_ROOT / "book_openings.txt"
    seed_file = SELFPLAY_ROOT / "seeding_hvh.txt"
    assert book_file.is_file() and seed_file.is_file(), "Run prepare_selfplay_openings.py first!"

    t_start = time.perf_counter()
    summary = {"config": vars(args), "rounds": {}}

    # ==================== ROUND 1: ARENA 1.0 (7 engines round-robin) ====================
    r1_engines = list(ENGINES.keys())
    r1_dir = SELFPLAY_ROOT / "Arena 1.0"
    r1_results: List[Tuple[str, str, int, int, int]] = []
    print(f"\n[ROUND 1 — ARENA 1.0] 7 engines, 21 pairs x ({args.r1_150}@150ms + {args.r1_500}@500ms)", flush=True)
    for a, b in itertools.combinations(r1_engines, 2):
        for mt, ng in ((150, args.r1_150), (500, args.r1_500)):
            if ng <= 0:
                continue
            rname = f"sp_r1_{a}_vs_{b}_{mt}ms"
            w, d, l = run_pair_match(rname, a, b, ng, mt, r1_dir, startpos=True, workers=args.workers)
            r1_results.append((a, b, w, d, l))

    r1_table = compute_bt_elo(r1_engines, r1_results)
    print_standings("ROUND 1 — ARENA 1.0 STANDINGS", r1_table)
    top4 = [r["engine"] for r in r1_table[:4]]
    print(f"[ROUND 1] Advancing Top 4 -> High dan Arena: {top4}", flush=True)
    summary["rounds"]["round1_arena_1_0"] = {"standings": r1_table, "top4": top4}
    (SELFPLAY_ROOT / "tournament_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

    # ==================== ROUND 2: HIGH DAN ARENA (Top 4 round-robin) ====================
    r2_dir = SELFPLAY_ROOT / "High dan Arena"
    r2_results: List[Tuple[str, str, int, int, int]] = []
    print(f"\n[ROUND 2 — HIGH DAN ARENA] Top 4 ({top4}), 6 pairs x ({args.r2_150}@150ms + {args.r2_500}@500ms)", flush=True)
    for a, b in itertools.combinations(top4, 2):
        for mt, ng in ((150, args.r2_150), (500, args.r2_500)):
            if ng <= 0:
                continue
            rname = f"sp_r2_{a}_vs_{b}_{mt}ms"
            w, d, l = run_pair_match(rname, a, b, ng, mt, r2_dir, startpos=True, workers=args.workers)
            r2_results.append((a, b, w, d, l))

    r2_table = compute_bt_elo(top4, r2_results)
    print_standings("ROUND 2 — HIGH DAN ARENA STANDINGS", r2_table)
    top2 = [r["engine"] for r in r2_table[:2]]
    print(f"[ROUND 2] Advancing Top 2 -> Final 3 Obstacles: {top2}", flush=True)
    summary["rounds"]["round2_high_dan_arena"] = {"standings": r2_table, "top2": top2}
    (SELFPLAY_ROOT / "tournament_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")

    # ==================== ROUND 3: FINAL — 3 OBSTACLES (Book / Seeding / Startpos) ====================
    f_a, f_b = top2[0], top2[1]
    obstacles = [
        ("Book", SELFPLAY_ROOT / "Book", False, book_file),
        ("Seeding", SELFPLAY_ROOT / "Seeding", False, seed_file),
        ("Startpos", SELFPLAY_ROOT / "High dan Arena" / "Final_Startpos", True, None),
    ]
    r3_results: List[Tuple[str, str, int, int, int]] = []
    r3_breakdown = {}
    print(f"\n[ROUND 3 — FINAL 3 OBSTACLES] {f_a} vs {f_b}", flush=True)
    for ob_name, ob_dir, is_sp, ob_file in obstacles:
        ob_w = ob_d = ob_l = 0
        for mt, ng in ((150, args.r3_150), (500, args.r3_500)):
            if ng <= 0:
                continue
            rname = f"sp_r3_{ob_name.lower()}_{f_a}_vs_{f_b}_{mt}ms"
            w, d, l = run_pair_match(rname, f_a, f_b, ng, mt, ob_dir, startpos=is_sp, openings=ob_file, workers=args.workers)
            r3_results.append((f_a, f_b, w, d, l))
            ob_w += w; ob_d += d; ob_l += l
        r3_breakdown[ob_name] = {"W": ob_w, "D": ob_d, "L": ob_l}
        print(f"  [OBSTACLE {ob_name}] {f_a} vs {f_b}: {ob_w}/{ob_d}/{ob_l}", flush=True)

    r3_table = compute_bt_elo(top2, r3_results)
    print_standings("ROUND 3 — FINAL 3 OBSTACLES STANDINGS", r3_table)
    champion = r3_table[0]["engine"]
    print(f"\n*** OFFICIAL CHAMPION: {champion} (total tournament wall={time.perf_counter()-t_start:.1f}s) ***", flush=True)
    summary["rounds"]["round3_final"] = {"standings": r3_table, "obstacles": r3_breakdown, "champion": champion}
    summary["champion"] = champion
    summary["champion_exe"] = str(ENGINES[champion])
    summary["wall_s"] = round(time.perf_counter() - t_start, 1)
    (SELFPLAY_ROOT / "tournament_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()

# Hōmei — Cờ Long Phượng (CLP) Chess Engine

**Hōmei** is a high-performance **C++20** engine for **Cờ Long Phượng (CLP)** communicating over the **`clp-ep 1`** protocol with [**ColongphuongGUI**](https://github.com/Tanhyka/ColongphuongGUI) and automated match/testing harnesses.

## Highlights (`v4-6_nnue` — `~2915 Elo`, anchor `v3-2 = 2000`)

- **SFNN / NNUE Evaluation (`CLP-HalfKA_hm`):**
  - `102,680` CLP-specific features (23 facing-aware top piece states, buried stack layers, reserve Long/Phượng, thermometer prisoners), `20` horizontally mirrored king buckets, `FT=256×2`, `SCReLU`, and `8` layer stacks indexed by prisoner count.
  - SIMD **AVX2** inference (`dot_u8_i8_512_x4`), FinnyTable, and lazy multi-ply accumulators (`760k–787k NPS` with MSVC PGO).
  - Automatically loads `homei_v46_gen1.nn` alongside `Homei_v4-6_nnue.exe` (configurable via `UseNNUE` / `NNUEFile`).
- **Compact Core & Branchless Bitboard (`584 B` `Position`):**
  - 128-bit branchless ray attacks + Hyperbola-Quintessence, zero-`apply_move` legality and bitboard Stack-SEE, `248 KB` Split Zobrist (`board_key` / `hand_key`), and 32-byte SWAR Cluster Transposition Table.
- **YaneuraOu-Inspired Search & Mate Solver:**
  - Move-Family `MovePicker`, 5-table Correction History, 1024-scale LMR + hindsight re-extension, thread-local facing-aware 1/2/4-ply Continuation History, Singular/Double/Multi-Cut, ProbCut, and SWAR Superior/Inferior Hand detection.
  - Persistent `condition_variable` Lazy SMP ThreadPool (`3.90×` scaling @ 4 threads) + Best-Thread Voting + **DF-PN Mate Search** (15+ ply).
- **Opening Book & Clock Management:**
  - Pre-game & early middlegame opening book (`2,898` lines / `1,968` unique Split-Zobrist positions covering 100% of ply-1 variations with mirror symmetry and `rep3` safety) plus ply-aware Byoyomi time control.
- **100% CLP Rules Compliance:** Full support for stacks (`MAX_STACK = 38`), directional rotations, drops, **Rule 4 (Thiên Nhật Thủ — avoidable 3-fold repetition loss)**, and **Rule 5 (Thiên Nhật Hòa — unavoidable 3-fold repetition draw)**.

## Pre-built Binaries

Download **`Homei_v4-6_nnue.exe`** and **`homei_v46_gen1.nn`** (place both in the same folder) from [**GitHub Releases**](https://github.com/Tanhyka/Homei/releases).

## Build from Source (CMake + C++20)

```powershell
cd HomeiEngine
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target homei selfplay_gen clp_fast_core
```

For MSVC Profile-Guided Optimization (PGO), configure with `-DHOMEI_PGO_MODE=GEN`, run a representative `bench 6 1000` workload, then rebuild with `-DHOMEI_PGO_MODE=USE`.

## Protocol (`clp-ep 1`)

Supports `clp`, `isready`, `setoption` (`Depth`, `Nodes`, `MultiPV`, `Threads`, `Hash`, `OwnBook`, `BookDepth`, `BookRandom`, `BookFile`, `UseNNUE`, `NNUEFile`), `position state64`, `go`, `info64`, `bestmove`, `stop`, `bench`, `nnuetest`, and `quit`.

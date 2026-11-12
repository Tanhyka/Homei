#pragma once

#include "movegen.h"
#include "types.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace homei {

// Generates all legal moves for `pos.side_to_move` that deliver check to the opponent's VUONG
// (direct drops, single/team checks, discovered checks, stack-split exposed checks, in-place rotations).
void generate_checks(Position& pos, MoveList& out);

// Generates all legal evasion moves when `pos.side_to_move` is in check
// (king moves, captures of a single checker, interpositions along a single sliding check ray).
// Falls back to generate_legal_moves if side to move is not currently in check.
void generate_evasions(Position& pos, MoveList& out);

// Fast checkmate / stalemate-while-checked predicate: returns true if `pos.side_to_move`
// (assumed in check) has at least one legal evasion.
bool has_any_legal_evasion(Position& pos);

struct MateResult {
    bool found = false;
    int mate_ply = 0; // total plies (Shogi-style: odd = attacker moves, 1 = immediate mate)
    uint64_t nodes = 0;
    bool aborted = false;
    uint32_t root_pn = 1;
    uint32_t root_dn = 1;
    std::vector<Move> pv;
};

struct RepHashEntry {
    uint64_t hash;
    int count;
};

// Bounded AND/OR Tsume solver up to `max_ply` plies from `root_pos`.
MateResult solve_mate(
    const Position& root_pos,
    int max_ply,
    int64_t time_budget_ms = 80,
    uint64_t max_nodes = 5000000ULL,
    const std::atomic<bool>* stop_flag = nullptr,
    const std::vector<RepHashEntry>* base_rep = nullptr
);

// Convenience signature matching spec.
bool solve_mate(const Position& root_pos, int max_ply, Move& out_best_move, int& out_mate_ply);

// Fast mate-in-1 scanner on static MoveList.
bool has_mate_in_1(
    Position& pos,
    Move& out_move,
    const uint64_t* path_hashes = nullptr,
    int ply = 0,
    const std::vector<RepHashEntry>* base_rep = nullptr
);

// Lightweight bounded df-pn for PV nodes with search-path history.
MateResult solve_mate_bounded(
    const Position& root_pos,
    int max_ply = 5,
    uint64_t max_nodes = 150,
    const std::atomic<bool>* stop_flag = nullptr,
    const std::vector<RepHashEntry>* base_rep = nullptr,
    const uint64_t* prior_path = nullptr,
    int prior_ply = 0
);

} // namespace homei

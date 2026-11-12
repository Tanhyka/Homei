#pragma once

#include "types.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace homei {

enum TTBound : uint8_t {
    TT_NONE  = 0,
    TT_EXACT = 1,
    TT_LOWER = 2, // beta cutoff (score >= beta)
    TT_UPPER = 3  // alpha fail-low (score <= alpha)
};

struct TTEntry {
    uint64_t key = 0;
    int16_t score = 0;
    int16_t static_eval = 0;
    Move best_move = Move::none();
    int8_t depth = -1;
    uint8_t bound = TT_NONE;
    uint8_t gen = 0;
};

struct TTBucket {
    TTEntry tier[2]; // tier[0]: depth-preferred, tier[1]: always-replace
};

inline int16_t score_to_tt(int score, int ply) {
    if (score >= SCORE_MATE - MAX_PLY) return static_cast<int16_t>(score + ply);
    if (score <= -(SCORE_MATE - MAX_PLY)) return static_cast<int16_t>(score - ply);
    return static_cast<int16_t>(score);
}

inline int score_from_tt(int16_t tt_score, int ply) {
    int s = static_cast<int>(tt_score);
    if (s >= SCORE_MATE - MAX_PLY) return s - ply;
    if (s <= -(SCORE_MATE - MAX_PLY)) return s + ply;
    return s;
}

class TranspositionTable {
public:
    explicit TranspositionTable(size_t size_mb = 64);

    void resize(size_t size_mb);
    void clear();
    void new_search() { ++generation_; }

    bool probe(uint64_t key, TTEntry& out_entry) const;
    void store(uint64_t key, int depth, int ply, int score, uint8_t bound, Move best_move);

private:
    std::vector<TTBucket> table_;
    size_t mask_ = 0;
    uint8_t generation_ = 0;
};

} // namespace homei

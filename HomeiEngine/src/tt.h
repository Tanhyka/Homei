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

#pragma pack(push, 1)
struct TTData {
    Move best_move;      // 6 bytes
    int16_t score;       // 2 bytes
    int16_t static_eval; // 2 bytes
    int8_t depth;        // 1 byte
    uint8_t bound;       // 1 byte
    uint8_t gen;         // 1 byte
    uint8_t pad[3];      // 3 bytes -> total 16 bytes
};
#pragma pack(pop)
static_assert(sizeof(TTData) == 16, "TTData must be exactly 16 bytes for 128-bit lockless payload");

// Lockless XOR storage: lock = key ^ d0 ^ d1
struct TTLocklessSlot {
    uint64_t lock = 0;
    uint64_t d0 = 0;
    uint64_t d1 = 0;
};

struct TTBucket {
    TTLocklessSlot tier[2]; // tier[0]: depth-preferred, tier[1]: always-replace
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
    uint8_t generation() const { return generation_; }

    bool probe(uint64_t key, TTEntry& out_entry) const;
    void store(uint64_t key, int depth, int ply, int score, uint8_t bound, Move best_move);

private:
    std::vector<TTBucket> table_;
    size_t mask_ = 0;
    uint8_t generation_ = 0;
};

} // namespace homei

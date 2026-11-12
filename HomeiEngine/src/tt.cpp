#include "tt.h"
#include <cstring>

namespace homei {

TranspositionTable::TranspositionTable(size_t size_mb) {
    resize(size_mb);
}

void TranspositionTable::resize(size_t size_mb) {
    if (size_mb < 1) size_mb = 1;
    size_t bytes = size_mb * 1024ULL * 1024ULL;
    size_t raw_buckets = bytes / sizeof(TTBucket);
    size_t pow2 = 1;
    while ((pow2 << 1) <= raw_buckets) {
        pow2 <<= 1;
    }
    table_.assign(pow2, TTBucket{});
    mask_ = pow2 - 1;
    generation_ = 0;
}

void TranspositionTable::clear() {
    std::fill(table_.begin(), table_.end(), TTBucket{});
    generation_ = 0;
}

bool TranspositionTable::probe(uint64_t key, TTEntry& out_entry) const {
    if (table_.empty()) return false;
    const TTBucket& b = table_[key & mask_];
    if (b.tier[0].bound != TT_NONE && b.tier[0].key == key) {
        out_entry = b.tier[0];
        return true;
    }
    if (b.tier[1].bound != TT_NONE && b.tier[1].key == key) {
        out_entry = b.tier[1];
        return true;
    }
    return false;
}

void TranspositionTable::store(uint64_t key, int depth, int ply, int score, uint8_t bound, Move best_move) {
    if (table_.empty()) return;
    TTBucket& b = table_[key & mask_];
    int16_t adj_score = score_to_tt(score, ply);

    // If key already in tier[0], update in place
    if (b.tier[0].bound == TT_NONE || b.tier[0].key == key) {
        if (best_move.is_none() && b.tier[0].key == key) {
            best_move = b.tier[0].best_move;
        }
        if (b.tier[0].bound == TT_NONE || depth >= b.tier[0].depth || bound == TT_EXACT || b.tier[0].gen != generation_) {
            b.tier[0].key = key;
            b.tier[0].score = adj_score;
            b.tier[0].best_move = best_move;
            b.tier[0].depth = static_cast<int8_t>(depth);
            b.tier[0].bound = bound;
            b.tier[0].gen = generation_;
        }
        return;
    }

    // Decide whether new entry replaces tier[0] (moving old tier[0] to tier[1]) or goes into tier[1]
    bool replace_tier0 = (b.tier[0].gen != generation_) ||
                         (depth >= b.tier[0].depth) ||
                         (bound == TT_EXACT && b.tier[0].bound != TT_EXACT);
    if (replace_tier0) {
        b.tier[1] = b.tier[0];
        b.tier[0].key = key;
        b.tier[0].score = adj_score;
        b.tier[0].best_move = best_move;
        b.tier[0].depth = static_cast<int8_t>(depth);
        b.tier[0].bound = bound;
        b.tier[0].gen = generation_;
    } else {
        if (best_move.is_none() && b.tier[1].key == key) {
            best_move = b.tier[1].best_move;
        }
        b.tier[1].key = key;
        b.tier[1].score = adj_score;
        b.tier[1].best_move = best_move;
        b.tier[1].depth = static_cast<int8_t>(depth);
        b.tier[1].bound = bound;
        b.tier[1].gen = generation_;
    }
}

} // namespace homei

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

static inline bool unpack_slot(const TTLocklessSlot& slot, uint64_t key, TTData& out_data) {
    uint64_t lock = slot.lock;
    uint64_t d0 = slot.d0;
    uint64_t d1 = slot.d1;
    if ((lock ^ d0 ^ d1) != key) return false;
    std::memcpy(&out_data, &d0, 8);
    std::memcpy(reinterpret_cast<char*>(&out_data) + 8, &d1, 8);
    return out_data.bound != TT_NONE;
}

static inline void pack_slot(TTLocklessSlot& slot, uint64_t key, const TTData& data) {
    uint64_t d0, d1;
    std::memcpy(&d0, &data, 8);
    std::memcpy(&d1, reinterpret_cast<const char*>(&data) + 8, 8);
    slot.d0 = d0;
    slot.d1 = d1;
    slot.lock = key ^ d0 ^ d1;
}

bool TranspositionTable::probe(uint64_t key, TTEntry& out_entry) const {
    if (table_.empty()) return false;
    const TTBucket& b = table_[key & mask_];
    TTData d;
    for (int t = 0; t < 2; ++t) {
        if (unpack_slot(b.tier[t], key, d)) {
            out_entry.key = key;
            out_entry.score = d.score;
            out_entry.static_eval = d.static_eval;
            out_entry.best_move = d.best_move;
            out_entry.depth = d.depth;
            out_entry.bound = d.bound;
            out_entry.gen = d.gen;
            return true;
        }
    }
    return false;
}

void TranspositionTable::store(uint64_t key, int depth, int ply, int score, uint8_t bound, Move best_move) {
    if (table_.empty()) return;
    TTBucket& b = table_[key & mask_];
    int16_t adj_score = score_to_tt(score, ply);

    TTData d0_cur, d1_cur;
    bool has_d0 = unpack_slot(b.tier[0], key, d0_cur);
    bool has_d1 = unpack_slot(b.tier[1], key, d1_cur);

    if (has_d0) {
        if (best_move.is_none()) {
            best_move = d0_cur.best_move;
        }
        if (depth >= d0_cur.depth || bound == TT_EXACT || d0_cur.gen != generation_) {
            TTData new_data;
            new_data.best_move = best_move;
            new_data.score = adj_score;
            new_data.static_eval = 0;
            new_data.depth = static_cast<int8_t>(depth);
            new_data.bound = bound;
            new_data.gen = generation_;
            new_data.pad[0] = new_data.pad[1] = new_data.pad[2] = 0;
            pack_slot(b.tier[0], key, new_data);
        }
        return;
    }

    if (has_d1) {
        if (best_move.is_none()) {
            best_move = d1_cur.best_move;
        }
        TTData new_data;
        new_data.best_move = best_move;
        new_data.score = adj_score;
        new_data.static_eval = 0;
        new_data.depth = static_cast<int8_t>(depth);
        new_data.bound = bound;
        new_data.gen = generation_;
        new_data.pad[0] = new_data.pad[1] = new_data.pad[2] = 0;
        pack_slot(b.tier[1], key, new_data);
        return;
    }

    TTData t0_any;
    std::memcpy(&t0_any, &b.tier[0].d0, 8);
    std::memcpy(reinterpret_cast<char*>(&t0_any) + 8, &b.tier[0].d1, 8);

    bool replace_tier0 = (b.tier[0].lock == 0) ||
                         (t0_any.gen != generation_) ||
                         (depth >= t0_any.depth) ||
                         (bound == TT_EXACT && t0_any.bound != TT_EXACT);

    TTData new_data;
    new_data.best_move = best_move;
    new_data.score = adj_score;
    new_data.static_eval = 0;
    new_data.depth = static_cast<int8_t>(depth);
    new_data.bound = bound;
    new_data.gen = generation_;
    new_data.pad[0] = new_data.pad[1] = new_data.pad[2] = 0;

    if (replace_tier0) {
        b.tier[1] = b.tier[0];
        pack_slot(b.tier[0], key, new_data);
    } else {
        pack_slot(b.tier[1], key, new_data);
    }
}

} // namespace homei

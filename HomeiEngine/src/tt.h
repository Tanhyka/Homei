#pragma once

#include "types.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#include <xmmintrin.h>
#elif defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace homei {

constexpr int16_t EVAL_NONE = 32001;

enum TTBound : uint8_t {
    TT_NONE  = 0,
    TT_EXACT = 1,
    TT_LOWER = 2, // beta cutoff (score >= beta)
    TT_UPPER = 3  // alpha fail-low (score <= alpha)
};

struct TTEntry {
    uint64_t key = 0;
    int16_t score = 0;
    int16_t static_eval = EVAL_NONE;
    Move best_move = Move::none();
    int8_t depth = -1;
    uint8_t bound = TT_NONE;
    uint8_t gen = 0;
};

// YaneuraOu-style 32-Byte Aligned SWAR Cluster (2 clusters per 64-byte cache line):
// - `keys`: three 21-bit tags packed in [0..20], [21..41], [42..62] (bit 0 of each tag == side_to_move!).
// - `data[3]`: three single-word 64-bit atomic slots (zero torn reads under Lazy SMP!):
//     bits [0..20]  : 21-bit lossless packed Move
//     bits [21..36] : 16-bit int16_t score
//     bits [37..51] : 15-bit static_eval (0 = EVAL_NONE, else clamp(-16383..+16383) + 16384)
//     bits [52..57] : 6-bit depth + 1 (-1..62)
//     bits [58..59] : 2-bit TTBound (0..3)
//     bits [60..63] : 4-bit generation (0..15)
struct alignas(32) TTCluster {
    uint64_t keys = 0;
    uint64_t data[3]{0, 0, 0};
};
static_assert(sizeof(TTCluster) == 32, "TTCluster must be exactly 32 bytes (2 per cache line)");

inline uint64_t mul_hi64(uint64_t a, uint64_t b) {
#if defined(_MSC_VER) && defined(_M_X64)
    return __umulh(a, b);
#else
    return static_cast<uint64_t>((static_cast<unsigned __int128>(a) * b) >> 64);
#endif
}

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
    void new_search() { generation_ = static_cast<uint8_t>((generation_ + 1) & 15); }
    uint8_t generation() const { return generation_; }

    inline size_t cluster_index(uint64_t key) const {
        return static_cast<size_t>(mul_hi64(key, cluster_count_));
    }

    inline void prefetch(uint64_t key) const {
        if (!table_.empty()) {
            const char* addr = reinterpret_cast<const char*>(&table_[cluster_index(key)]);
#if defined(_MSC_VER) || defined(__SSE__)
            _mm_prefetch(addr, _MM_HINT_T0);
#else
            __builtin_prefetch(addr, 0, 3);
#endif
        }
    }

    bool probe(uint64_t key, TTEntry& out_entry) const;
    void store(uint64_t key, int depth, int ply, int score, uint8_t bound, Move best_move, int16_t static_eval = EVAL_NONE);
    void store_static_eval(uint64_t key, int16_t static_eval);

private:
    std::vector<TTCluster> table_;
    uint64_t cluster_count_ = 0;
    uint8_t generation_ = 0;
};

} // namespace homei

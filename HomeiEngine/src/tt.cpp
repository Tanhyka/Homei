#include "tt.h"
#include "eval.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>

namespace homei {

namespace {

constexpr uint64_t TAG_MASK       = 0x1FFFFFULL;            // 21 bits
constexpr uint64_t SWAR_BROADCAST = 0x0000040000200001ULL;  // bits 0, 21, 42
constexpr uint64_t SWAR_LO_BITS   = 0x0000040000200001ULL;  // bits 0, 21, 42
constexpr uint64_t SWAR_HI_BITS   = 0x4000020000100000ULL;  // bits 20, 41, 62
constexpr uint64_t SWAR_ADD_CONST = SWAR_HI_BITS - SWAR_LO_BITS;

static_assert(std::atomic_ref<uint64_t>::required_alignment <= alignof(TTCluster),
              "TTCluster fields must satisfy atomic_ref<uint64_t> alignment");

inline uint64_t atomic_load64(const uint64_t& v) {
    return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(v)).load(std::memory_order_relaxed);
}

inline void atomic_store64(uint64_t& v, uint64_t val) {
    std::atomic_ref<uint64_t>(v).store(val, std::memory_order_relaxed);
}

// 21-bit tag orthogonal to mul_hi64(key, cluster_count_) (which uses top ~21..25 bits of key),
// preserving side-to-move isolation in bit 0 and never returning 0 (empty slot sentinel).
inline uint64_t make_tag21(uint64_t key) {
    uint64_t t = (((key >> 1) ^ (key >> 22)) << 1) | (key & 1ULL);
    t &= TAG_MASK;
    if (t == 0) t = 2ULL;
    return t;
}

// Returns matching slot index [0..2] via borrow-free 3-lane SWAR, or -1 on miss.
inline int swar_find_slot(uint64_t keys, uint64_t tag) {
    const uint64_t x = keys ^ (tag * SWAR_BROADCAST);
    const uint64_t z = ~(((x & ~SWAR_HI_BITS) + SWAR_ADD_CONST) | x | ~SWAR_HI_BITS);
    if (z == 0) return -1;
    if (z & (1ULL << 20)) return 0;
    if (z & (1ULL << 41)) return 1;
    return 2;
}

inline uint64_t get_lane_tag(uint64_t keys, int slot) {
    return (keys >> (slot * 21)) & TAG_MASK;
}

inline void set_lane_tag_atomic(uint64_t& keys_ref, int slot, uint64_t tag) {
    const int shift = slot * 21;
    const uint64_t lane_mask = TAG_MASK << shift;
    const uint64_t new_bits = (tag & TAG_MASK) << shift;
    std::atomic_ref<uint64_t> aref(keys_ref);
    uint64_t cur = aref.load(std::memory_order_relaxed);
    while (!aref.compare_exchange_weak(cur, (cur & ~lane_mask) | new_bits, std::memory_order_relaxed)) {}
}

inline uint32_t pack_move21(const Move& m) {
    if (m.is_none()) return 0U;
    uint32_t kind = static_cast<uint32_t>(m.kind) & 3U;
    uint32_t src_u = (m.src >= 0 && m.src < NUM_SQUARES) ? static_cast<uint32_t>(m.src) : 81U;
    uint32_t dst_u = (m.dst >= 0 && m.dst < NUM_SQUARES) ? static_cast<uint32_t>(m.dst) : 81U;
    uint32_t pt = static_cast<uint32_t>(m.ptype) & 7U;
    uint32_t aux = (m.kind == MoveKind::Drop)
        ? (static_cast<uint32_t>(m.facing >= 0 ? m.facing : 0) & 3U)
        : (static_cast<uint32_t>(m.rotate_after) & 3U);
    return kind | (src_u << 2) | (dst_u << 9) | (pt << 16) | (aux << 19);
}

inline Move unpack_move21(uint32_t bits) {
    if (bits == 0U) return Move::none();
    Move m;
    m.kind = static_cast<MoveKind>(bits & 3U);
    uint32_t src_u = (bits >> 2) & 0x7FU;
    uint32_t dst_u = (bits >> 9) & 0x7FU;
    m.src = (src_u < NUM_SQUARES) ? static_cast<int8_t>(src_u) : -1;
    m.dst = (dst_u < NUM_SQUARES) ? static_cast<int8_t>(dst_u) : -1;
    m.ptype = static_cast<uint8_t>((bits >> 16) & 7U);
    uint32_t aux = (bits >> 19) & 3U;
    if (m.kind == MoveKind::Drop) {
        m.facing = static_cast<int8_t>(aux);
        m.rotate_after = 0;
    } else {
        m.facing = -1;
        m.rotate_after = static_cast<uint8_t>(aux);
    }
    return m;
}

inline uint64_t pack_slot64(
    const Move& best_move,
    int16_t score,
    int16_t static_eval,
    int depth,
    uint8_t bound,
    uint8_t gen
) {
    uint64_t mv_bits = static_cast<uint64_t>(pack_move21(best_move));
    uint64_t sc_bits = static_cast<uint64_t>(static_cast<uint16_t>(score));
    uint64_t ev_bits = 0;
    if (static_eval != EVAL_NONE) {
        int clamped = std::clamp<int>(static_eval, -16383, 16383);
        ev_bits = static_cast<uint64_t>(clamped + 16384); // 1..32767
    }
    uint64_t d_bits = static_cast<uint64_t>(std::clamp(depth, -1, 62) + 1); // 0..63
    uint64_t b_bits = static_cast<uint64_t>(bound & 3U);
    uint64_t g_bits = static_cast<uint64_t>(gen & 15U);
    return mv_bits
         | (sc_bits << 21)
         | (ev_bits << 37)
         | (d_bits  << 52)
         | (b_bits  << 58)
         | (g_bits  << 60);
}

inline void unpack_slot64(uint64_t word, TTEntry& out) {
    out.best_move = unpack_move21(static_cast<uint32_t>(word & 0x1FFFFFULL));
    out.score = static_cast<int16_t>(static_cast<uint16_t>((word >> 21) & 0xFFFFULL));
    uint32_t ev_bits = static_cast<uint32_t>((word >> 37) & 0x7FFFULL);
    out.static_eval = (ev_bits == 0U) ? EVAL_NONE : static_cast<int16_t>(static_cast<int>(ev_bits) - 16384);
    out.depth = static_cast<int8_t>(static_cast<int>((word >> 52) & 0x3FULL) - 1);
    out.bound = static_cast<uint8_t>((word >> 58) & 3ULL);
    out.gen = static_cast<uint8_t>((word >> 60) & 0xFULL);
}

inline int slot_depth(uint64_t word) {
    return static_cast<int>((word >> 52) & 0x3FULL) - 1;
}

inline uint8_t slot_bound(uint64_t word) {
    return static_cast<uint8_t>((word >> 58) & 3ULL);
}

inline uint8_t slot_gen(uint64_t word) {
    return static_cast<uint8_t>((word >> 60) & 0xFULL);
}

inline int16_t slot_static_eval(uint64_t word) {
    uint32_t ev_bits = static_cast<uint32_t>((word >> 37) & 0x7FFFULL);
    return (ev_bits == 0U) ? EVAL_NONE : static_cast<int16_t>(static_cast<int>(ev_bits) - 16384);
}

inline int tt_relative_age(uint8_t cur_gen, uint8_t entry_gen) {
    return static_cast<int>((cur_gen - entry_gen) & 15U);
}

inline int tt_replace_value(int depth, uint8_t bound, int age) {
    return depth + (bound == TT_EXACT ? 2 : 0) - 4 * age;
}

} // namespace

TranspositionTable::TranspositionTable(size_t size_mb) {
    if (size_mb > 0) {
        resize(size_mb);
    }
}

void TranspositionTable::resize(size_t size_mb) {
    if (size_mb < 1) size_mb = 1;
    size_t bytes = size_mb * 1024ULL * 1024ULL;
    size_t num_clusters = bytes / sizeof(TTCluster);
    if (num_clusters < 1024) num_clusters = 1024;
    std::vector<TTCluster>(num_clusters).swap(table_);
    cluster_count_ = static_cast<uint64_t>(num_clusters);
    generation_ = 0;
}

void TranspositionTable::clear() {
    std::fill(table_.begin(), table_.end(), TTCluster{});
    generation_ = 0;
}

bool TranspositionTable::probe(uint64_t key, TTEntry& out_entry) const {
    if (table_.empty()) return false;
#ifdef HOMEI_PROFILE
    SearchProfile& prof = get_search_profile();
    const bool prof_on = prof.enabled;
    std::chrono::steady_clock::time_point t0;
    if (prof_on) {
        ++prof.tt_probes;
        t0 = std::chrono::steady_clock::now();
    }
#endif

    const TTCluster& c = table_[cluster_index(key)];
    const uint64_t tag = make_tag21(key);
    const uint64_t keys = atomic_load64(c.keys);
    const int slot = swar_find_slot(keys, tag);
    bool hit = false;
    if (slot >= 0) {
        const uint64_t word = atomic_load64(c.data[slot]);
        // Re-verify tag after reading data to prevent cross-slot tear under Lazy SMP
        if (get_lane_tag(atomic_load64(c.keys), slot) == tag) {
            unpack_slot64(word, out_entry);
            if (out_entry.bound != TT_NONE) {
                out_entry.key = key;
                hit = true;
            }
        }
    }

#ifdef HOMEI_PROFILE
    if (prof_on) {
        auto t1 = std::chrono::steady_clock::now();
        prof.tt_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
        );
    }
#endif
    return hit;
}

void TranspositionTable::store(uint64_t key, int depth, int ply, int score, uint8_t bound, Move best_move, int16_t static_eval) {
    if (table_.empty()) return;
#ifdef HOMEI_PROFILE
    SearchProfile& prof = get_search_profile();
    const bool prof_on = prof.enabled;
    std::chrono::steady_clock::time_point t0;
    if (prof_on) {
        ++prof.tt_stores;
        t0 = std::chrono::steady_clock::now();
    }
    struct TTProfGuard {
        bool on;
        std::chrono::steady_clock::time_point t0;
        ~TTProfGuard() {
            if (on) {
                auto t1 = std::chrono::steady_clock::now();
                get_search_profile().tt_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
                );
            }
        }
    } guard{prof_on, t0};
#endif

    TTCluster& c = table_[cluster_index(key)];
    const uint64_t tag = make_tag21(key);
    const int16_t adj_score = score_to_tt(score, ply);

    const uint64_t keys = atomic_load64(c.keys);
    const int hit_slot = swar_find_slot(keys, tag);
    if (hit_slot >= 0) {
        const uint64_t old_word = atomic_load64(c.data[hit_slot]);
        TTEntry cur;
        unpack_slot64(old_word, cur);
        if (best_move.is_none()) {
            best_move = cur.best_move;
        }
        int16_t eff_eval = (static_eval != EVAL_NONE) ? static_eval : cur.static_eval;
        int age = tt_relative_age(generation_, cur.gen);
        bool shallow_over_deep = (depth <= 0 && cur.depth > 0 && age <= 2);
        bool should_replace = !shallow_over_deep && (
            depth >= cur.depth ||
            (bound == TT_EXACT && depth > 0 && depth + 2 >= cur.depth) ||
            age >= 2
        );
        if (should_replace) {
            uint64_t nw = pack_slot64(best_move, adj_score, eff_eval, depth, bound, generation_);
            atomic_store64(c.data[hit_slot], nw);
        } else if (static_eval != EVAL_NONE && cur.static_eval == EVAL_NONE) {
            uint64_t nw = pack_slot64(cur.best_move, cur.score, static_eval, cur.depth, cur.bound, cur.gen);
            atomic_store64(c.data[hit_slot], nw);
        }
        return;
    }

    // Pick lowest-value victim among the 3 slots in this 32B cluster
    int victim = 0;
    int worst_val = 1000000;
    for (int s = 0; s < 3; ++s) {
        const uint64_t st = get_lane_tag(keys, s);
        const uint64_t sw = atomic_load64(c.data[s]);
        const uint8_t sb = slot_bound(sw);
        if (st == 0 || sb == TT_NONE) {
            victim = s;
            break;
        }
        const int sd = slot_depth(sw);
        const int sa = tt_relative_age(generation_, slot_gen(sw));
        // Protect recent depth>0 entries from being evicted by depth<=0 if another weaker slot exists
        int val = tt_replace_value(sd, sb, sa);
        if (depth <= 0 && sd > 0 && sa <= 2) {
            val += 32;
        }
        if (val < worst_val) {
            worst_val = val;
            victim = s;
        }
    }

    const uint64_t nw = pack_slot64(best_move, adj_score, static_eval, depth, bound, generation_);
    atomic_store64(c.data[victim], nw);
    set_lane_tag_atomic(c.keys, victim, tag);
}

void TranspositionTable::store_static_eval(uint64_t key, int16_t static_eval) {
    if (table_.empty() || static_eval == EVAL_NONE) return;
    TTCluster& c = table_[cluster_index(key)];
    const uint64_t tag = make_tag21(key);
    const uint64_t keys = atomic_load64(c.keys);

    const int hit_slot = swar_find_slot(keys, tag);
    if (hit_slot >= 0) {
        const uint64_t old_word = atomic_load64(c.data[hit_slot]);
        if (slot_static_eval(old_word) != static_eval) {
            TTEntry cur;
            unpack_slot64(old_word, cur);
            uint64_t nw = pack_slot64(cur.best_move, cur.score, static_eval, cur.depth, cur.bound, cur.gen);
            atomic_store64(c.data[hit_slot], nw);
        }
        return;
    }

    // Only write a static-eval-only entry (depth = -1) into an empty, depth < 0, or old (age >= 4) slot.
    int victim = -1;
    for (int s = 0; s < 3; ++s) {
        const uint64_t st = get_lane_tag(keys, s);
        const uint64_t sw = atomic_load64(c.data[s]);
        const uint8_t sb = slot_bound(sw);
        if (st == 0 || sb == TT_NONE) {
            victim = s;
            break;
        }
        if (slot_depth(sw) < 0 || tt_relative_age(generation_, slot_gen(sw)) >= 4) {
            victim = s;
        }
    }
    if (victim >= 0) {
        const uint64_t nw = pack_slot64(Move::none(), 0, static_eval, -1, TT_UPPER, generation_);
        atomic_store64(c.data[victim], nw);
        set_lane_tag_atomic(c.keys, victim, tag);
    }
}

} // namespace homei

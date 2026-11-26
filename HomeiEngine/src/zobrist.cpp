#include "zobrist.h"
#include <mutex>

namespace homei {

uint64_t zobrist_cell[ZOBRIST_FAST_DEPTH][NUM_SQUARES][64];
uint64_t zobrist_prisoner_unit[2][8];
uint64_t zobrist_reserve[2][4];
uint64_t zobrist_side = 1ULL;

namespace {

std::once_flag g_zobrist_once;

uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

inline uint8_t canonical_code64(const Piece& p) {
    uint8_t f = can_rotate(p.type) ? p.facing : initial_facing(static_cast<Side>(p.owner));
    return static_cast<uint8_t>(((p.owner * 8 + p.type) * 4) + f);
}

} // namespace

void init_zobrist() {
    std::call_once(g_zobrist_once, []() {
        uint64_t seed = 0x484F4D45495F5635ULL; // "HOMEI_V5"
        for (int d = 0; d < ZOBRIST_FAST_DEPTH; ++d) {
            for (int sq = 0; sq < NUM_SQUARES; ++sq) {
                for (int c = 0; c < 64; ++c) {
                    zobrist_cell[d][sq][c] = splitmix64(seed) & ~1ULL;
                }
            }
        }
        for (int s = 0; s < 2; ++s) {
            for (int pt = 0; pt < 8; ++pt) {
                zobrist_prisoner_unit[s][pt] = splitmix64(seed) & ~1ULL;
            }
            zobrist_reserve[s][0] = 0ULL;
            for (int m = 1; m < 4; ++m) {
                zobrist_reserve[s][m] = splitmix64(seed) & ~1ULL;
            }
        }
        zobrist_side = splitmix64(seed) | 1ULL;
    });
}

void compute_zobrist_split(const Position& pos, uint64_t& out_board_key, uint64_t& out_hand_key) {
    uint64_t bk = 0;
    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        uint8_t h = pos.height[sq];
        if (h == 0) continue;
        bk ^= zobrist_cell_key(static_cast<uint8_t>(h - 1), sq, canonical_code64(pos.top_piece[sq]));
        if (h >= 2) {
            int d = static_cast<int>(h) - 2;
            for (uint8_t idx = pos.buried_head[sq]; idx != BURIED_NONE && d >= 0; idx = pos.buried_next[idx], --d) {
                bk ^= zobrist_cell_key(static_cast<uint8_t>(d), sq, canonical_code64(pos.buried_piece[idx]));
            }
        }
    }
    if (pos.side_to_move == XANH) {
        bk ^= zobrist_side;
    }

    uint64_t hk = 0;
    for (int s = 0; s < 2; ++s) {
        hk += zobrist_reserve[s][pos.reserve[s] & 3];
        for (int pt = 0; pt < 8; ++pt) {
            uint8_t cnt = pos.prisoners[s][pt];
            if (cnt > 0) {
                hk += static_cast<uint64_t>(cnt) * zobrist_prisoner_unit[s][pt];
            }
        }
    }
    out_board_key = bk;
    out_hand_key = hk & ~1ULL; // invariant: bit 0 is always 0 (sum of even numbers is even!)
}

uint64_t compute_zobrist(const Position& pos) {
    uint64_t bk = 0, hk = 0;
    compute_zobrist_split(pos, bk, hk);
    return bk ^ hk;
}

} // namespace homei

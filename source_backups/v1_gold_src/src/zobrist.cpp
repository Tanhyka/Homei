#include "zobrist.h"

namespace homei {

uint64_t zobrist_cell[MAX_STACK_SIZE][NUM_SQUARES][64];
uint64_t zobrist_prisoner[2][8][MAX_TOTAL_PIECES + 1];
uint64_t zobrist_reserve[2][4];
uint64_t zobrist_side = 0;

namespace {

bool g_zobrist_init = false;

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
    if (g_zobrist_init) return;
    uint64_t seed = 0x484F4D45495F434CULL; // "HOMEI_CL"
    for (int d = 0; d < MAX_STACK_SIZE; ++d) {
        for (int sq = 0; sq < NUM_SQUARES; ++sq) {
            for (int c = 0; c < 64; ++c) {
                zobrist_cell[d][sq][c] = splitmix64(seed);
            }
        }
    }
    for (int s = 0; s < 2; ++s) {
        for (int pt = 0; pt < 8; ++pt) {
            zobrist_prisoner[s][pt][0] = 0;
            for (int cnt = 1; cnt <= MAX_TOTAL_PIECES; ++cnt) {
                zobrist_prisoner[s][pt][cnt] = splitmix64(seed);
            }
        }
        zobrist_reserve[s][0] = 0;
        for (int m = 1; m < 4; ++m) {
            zobrist_reserve[s][m] = splitmix64(seed);
        }
    }
    zobrist_side = splitmix64(seed);
    g_zobrist_init = true;
}

uint64_t compute_zobrist(const Position& pos) {
    if (!g_zobrist_init) init_zobrist();
    uint64_t h = 0;
    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        const Cell& c = pos.cell[sq];
        for (uint8_t d = 0; d < c.size; ++d) {
            h ^= zobrist_cell[d][sq][canonical_code64(c.stack[d])];
        }
    }
    for (int s = 0; s < 2; ++s) {
        h ^= zobrist_reserve[s][pos.reserve[s] & 3];
        for (int pt = 0; pt < 8; ++pt) {
            uint8_t cnt = pos.prisoners[s][pt];
            if (cnt <= MAX_TOTAL_PIECES) {
                h ^= zobrist_prisoner[s][pt][cnt];
            }
        }
    }
    if (pos.side_to_move == XANH) {
        h ^= zobrist_side;
    }
    return h;
}

} // namespace homei

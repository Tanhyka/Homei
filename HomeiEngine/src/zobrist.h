#pragma once

#include "types.h"

namespace homei {

// Compact Split Zobrist (~248 KB in L2, down from 1.50 MB in v4.2):
// - All board/hand keys have bit 0 == 0.
// - `zobrist_side` has bit 0 == 1, so `board_key & 1 == side_to_move` and `zobrist & 1 == side_to_move`.
// - `board_key`: XOR of board pieces (and `zobrist_side` when XANH).
// - `hand_key`: additive sum of prisoner units + reserve masks (commutatively supports add/sub and Superior Hand detection!).
// - `pos.zobrist = pos.board_key ^ pos.hand_key`.
constexpr int ZOBRIST_FAST_DEPTH = 6;

extern uint64_t zobrist_cell[ZOBRIST_FAST_DEPTH][NUM_SQUARES][64];
extern uint64_t zobrist_prisoner_unit[2][8];
extern uint64_t zobrist_reserve[2][4];
extern uint64_t zobrist_side;

inline uint64_t zobrist_cell_key(uint8_t depth, int sq, uint8_t code64) {
    if (depth < ZOBRIST_FAST_DEPTH) {
        return zobrist_cell[depth][sq][code64];
    }
    uint64_t z = zobrist_cell[ZOBRIST_FAST_DEPTH - 1][sq][code64] ^
                 (static_cast<uint64_t>(depth) * 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (z ^ (z >> 31)) & ~1ULL;
}

void init_zobrist();
void compute_zobrist_split(const Position& pos, uint64_t& out_board_key, uint64_t& out_hand_key);
uint64_t compute_zobrist(const Position& pos);

inline void refresh_zobrist(Position& pos) {
    compute_zobrist_split(pos, pos.board_key, pos.hand_key);
    pos.zobrist = pos.board_key ^ pos.hand_key;
}

} // namespace homei

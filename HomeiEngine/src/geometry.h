#pragma once

#include "types.h"
#include <utility>

namespace homei {

struct MoveVec {
    int8_t dr;
    int8_t dc;
    bool sliding;
};

struct PieceMoveTable {
    uint8_t count = 0;
    MoveVec vecs[8]{};
};

// Rotate (dr, dc) clockwise `times` (0..3) times
constexpr inline std::pair<int, int> rotate_cw(int dr, int dc, int times) {
    int t = ((times % 4) + 4) % 4;
    if (t == 1) return {dc, -dr};
    if (t == 2) return {-dr, -dc};
    if (t == 3) return {-dc, dr};
    return {dr, dc};
}

void init_geometry();

// Returns the rotated movement vectors for (ptype, facing) in exact MOVES_LOCAL order
const PieceMoveTable& get_piece_moves(uint8_t ptype, uint8_t facing);

} // namespace homei

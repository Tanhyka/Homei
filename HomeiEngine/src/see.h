#pragma once

#include "types.h"

namespace homei {

// Base piece material values in centipawns (Section 4.3).
constexpr inline int piece_material_value(uint8_t pt) {
    constexpr int vals[8] = {
        10000, // VUONG
        420,   // TUONG
        320,   // SI
        360,   // KI
        280,   // XA
        600,   // LONG
        520,   // PHUONG
        100    // BINH
    };
    return (pt < 8) ? vals[pt] : 0;
}

// Single Source of Truth for evaluation piece values (centipawns)
constexpr inline int eval_piece_board_value(uint8_t pt) {
    if (pt == VUONG) return 0;
    return piece_material_value(pt);
}

constexpr inline int eval_piece_reserve_value(uint8_t pt) {
    if (pt == LONG || pt == PHUONG) {
        return piece_material_value(pt);
    }
    return -1;
}

constexpr inline int eval_piece_prisoner_value(uint8_t pt) {
    if (pt == VUONG || pt >= 8) return -1;
    int v = piece_material_value(pt);
    return (v * 9) / 10 + (v / 14);
}

constexpr inline int eval_start_side_total() {
    return 1 * eval_piece_board_value(VUONG)
         + 2 * eval_piece_board_value(TUONG)
         + 2 * eval_piece_board_value(SI)
         + 2 * eval_piece_board_value(KI)
         + 2 * eval_piece_board_value(XA)
         + 9 * eval_piece_board_value(BINH)
         + 1 * eval_piece_reserve_value(LONG)
         + 1 * eval_piece_reserve_value(PHUONG);
}

inline int cell_material_value(const Cell& c) {
    int sum = 0;
    for (uint8_t i = 0; i < c.size; ++i) {
        sum += piece_material_value(c.stack[i].type);
    }
    return sum;
}

// Returns true if `m` is a capture move (moves onto an enemy-occupied square).
inline bool is_capture_move(const Position& pos, const Move& m) {
    if (m.kind != MoveKind::Single && m.kind != MoveKind::Team) return false;
    if (m.dst < 0 || m.dst >= NUM_SQUARES) return false;
    const Cell& dst = pos.cell[m.dst];
    return !dst.empty() && dst.owner() != static_cast<Side>(pos.side_to_move);
}

// Stack-SEE estimate (Section 4.4) for a capture move `m` in `pos`.
// Returns estimated material gain in centipawns (of raw piece values).
int stack_see_estimate(const Position& pos, const Move& m);

// Evaluates worst-case SEE penalty on square `sq` owned by `owner` when `enemy` is to move.
// Used for hanging-piece evaluation (Section 4.3 item 4).
int square_hanging_loss(const Position& pos, int sq);

} // namespace homei

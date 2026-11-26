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

// Exchange swing of capturing/losing a piece on the board:
// In CLP all non-VUONG captured pieces (including LONG and PHUONG) transfer
// to the capturer's prisoner hand (~1.97x V = board value + prisoner value).
constexpr inline int piece_exchange_swing(uint8_t pt) {
    int v = piece_material_value(pt);
    if (pt == VUONG) return v;
    int pval = eval_piece_prisoner_value(pt);
    return (pval > 0) ? (v + pval) : v;
}

inline int cell_exchange_swing(const Position& pos, int sq) {
    uint8_t h = pos.height[sq];
    if (h == 0) return 0;
    int sum = piece_exchange_swing(pos.top_piece[sq].type);
    if (h >= 2) {
        for (uint8_t idx = pos.buried_head[sq]; idx != BURIED_NONE; idx = pos.buried_next[idx]) {
            sum += piece_exchange_swing(pos.buried_piece[idx].type);
        }
    }
    return sum;
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

inline int cell_material_value(const Position& pos, int sq) {
    uint8_t h = pos.height[sq];
    if (h == 0) return 0;
    int sum = piece_material_value(pos.top_piece[sq].type);
    if (h >= 2) {
        for (uint8_t idx = pos.buried_head[sq]; idx != BURIED_NONE; idx = pos.buried_next[idx]) {
            sum += piece_material_value(pos.buried_piece[idx].type);
        }
    }
    return sum;
}

// Returns true if `m` is a capture move (moves onto an enemy-occupied square).
inline bool is_capture_move(const Position& pos, const Move& m) {
    if (m.kind != MoveKind::Single && m.kind != MoveKind::Team) return false;
    if (m.dst < 0 || m.dst >= NUM_SQUARES) return false;
    return pos.height[m.dst] > 0 && pos.top_piece[m.dst].owner != pos.side_to_move;
}

// Pure-bitboard Stack-SEE estimate (zero apply_move/undo_move, zero Position copy, stack X-ray & pin aware).
int stack_see_estimate(const Position& pos, const Move& m);

// Pure-bitboard threshold SEE test: returns true iff stack_see_estimate(pos, m) >= threshold (with early exit).
bool see_ge(const Position& pos, const Move& m, int threshold);

// Evaluates worst-case hanging-piece penalty on square `sq`.
// Fast overload uses precomputed `bb_att[2]` from eval to short-circuit undefended pieces in O(1).
int square_hanging_loss(const Position& pos, int sq);
int square_hanging_loss_fast(const Position& pos, int sq, const Bitboard bb_att[2]);

} // namespace homei

#pragma once

#include "types.h"

namespace homei {

extern uint64_t zobrist_cell[MAX_STACK_SIZE][NUM_SQUARES][64];
extern uint64_t zobrist_prisoner[2][8][MAX_TOTAL_PIECES + 1];
extern uint64_t zobrist_reserve[2][4];
extern uint64_t zobrist_side;

void init_zobrist();
uint64_t compute_zobrist(const Position& pos);

} // namespace homei

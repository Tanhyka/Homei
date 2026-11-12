#pragma once

#include "types.h"

namespace homei {

// Evaluates position from the perspective of `pos.side_to_move` (in centipawns).
int evaluate(const Position& pos);

// Evaluates position from the perspective of `perspective` (CAM or XANH).
int evaluate_for_side(const Position& pos, Side perspective);

// Symmetry transforms for unit verification (Section 4.3 item 7).
Position mirror_horizontal(const Position& pos);
Position flip_colors_180(const Position& pos);

// Diagnostic piece values query (Search Audit Section 3)
void print_piecevalues(std::ostream& os);

} // namespace homei

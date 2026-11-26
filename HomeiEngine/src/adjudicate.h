#pragma once

#include "types.h"
#include <string>

namespace homei {

// Evaluates terminal/ongoing status on GameState (Section 2.6 + Điều 5 Thiên Nhật Hòa),
// prioritizing repetition3 / repetition3_draw before checkmate/pat.
bool adjudicate_state(GameState& st, std::string& err, bool unavoidable_rep3 = false);

// Applies a legal move to GameState, updating ply, repetition history, and result adjudication.
bool apply_move_to_state(GameState& st, const Move& m, std::string& err);

} // namespace homei

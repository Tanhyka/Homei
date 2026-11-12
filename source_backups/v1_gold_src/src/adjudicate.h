#pragma once

#include "types.h"
#include <string>

namespace homei {

// Evaluates terminal/ongoing status on GameState (Section 2.6), prioritizing repetition3 before checkmate/pat.
bool adjudicate_state(GameState& st, std::string& err);

// Applies a legal move to GameState, updating ply, repetition history, and result adjudication.
bool apply_move_to_state(GameState& st, const Move& m, std::string& err);

} // namespace homei

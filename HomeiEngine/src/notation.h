#pragma once

#include "types.h"
#include <string>
#include <string_view>

namespace homei {

// Formats a move in canonical CLP notation (Section 2.5), matching rules.Move.canonical_notation(board).
std::string format_canonical_notation(const Position& pos, const Move& m);

// Parses a canonical (or unambiguous compact) notation string in the given position into a legal Move.
bool parse_canonical_notation(const Position& pos, std::string_view text, Move& out_move, std::string& err);

} // namespace homei

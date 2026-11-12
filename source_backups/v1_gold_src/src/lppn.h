#pragma once

#include "types.h"
#include <string>
#include <string_view>

namespace homei {

// Validate position structural rules and update king_sq.
bool validate_position(Position& pos, bool strict, std::string& err);

// Parse LPPN string into Position and ply.
// Validates rules & strict constraints (1 king per side, <= 40 total pieces, etc.).
bool parse_lppn(std::string_view text, Position& out_pos, int& out_ply, std::string& err, bool strict = true);

// Format Position and ply into canonical LPPN v0.2 string (lppn1 ...).
bool format_lppn(const Position& pos, int ply, std::string& out_text, std::string& err, bool strict = false);

// Helper to initialize the standard opening position
void setup_initial_position(Position& pos);

} // namespace homei

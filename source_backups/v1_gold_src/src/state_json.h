#pragma once

#include "json.h"
#include "types.h"
#include <string>
#include <string_view>

namespace homei {

bool decode_position_json(const JsonValue& v, Position& out_pos, std::string& err);
JsonValue encode_position_json(const Position& pos);

bool decode_move_json(const JsonValue& v, Move& out_move, std::string& out_notation, std::string& err);
JsonValue encode_move_json(const Move& m, const std::string& notation);

bool decode_state_json(const JsonValue& v, GameState& out_state, std::string& err);
JsonValue encode_state_json(const GameState& state);

bool parse_state_json_str(std::string_view json_str, GameState& out_state, std::string& err);
std::string format_move_json_str(const Move& m, const std::string& notation);

// Helper to create a base GameState from a Position and ply (with 1 repetition entry count=1)
GameState make_base_state(const Position& pos, int ply);

} // namespace homei

#include "notation.h"
#include "attack.h"
#include "movegen.h"

namespace homei {

std::string format_canonical_notation(const Position& pos, const Move& m) {
    uint8_t ptype = m.ptype;
    uint8_t moving_owner = pos.side_to_move;
    uint8_t moving_facing = 0;

    if (m.kind != MoveKind::Drop && m.src >= 0 && m.src < NUM_SQUARES && pos.height[m.src] > 0) {
        const Piece top = pos.top_piece[m.src];
        ptype = top.type;
        moving_owner = top.owner;
        moving_facing = top.facing;
    }

    std::string out;
    out.reserve(12);
    if (m.kind == MoveKind::Team) {
        out.push_back('[');
        out.push_back(piece_code_char(ptype));
        out.push_back(']');
    } else {
        out.push_back(piece_code_char(ptype));
    }

    if (m.kind == MoveKind::Drop) {
        out.push_back('*');
    }

    int target_sq = (m.dst >= 0) ? m.dst : m.src;
    if (target_sq >= 0 && target_sq < NUM_SQUARES) {
        out += sq_to_str(target_sq);
    }

    if (can_rotate(ptype)) {
        int curr_f = (m.kind == MoveKind::Drop) ? static_cast<int>(m.facing) : static_cast<int>(moving_facing);
        int final_f = (curr_f + static_cast<int>(m.rotate_after)) & 3;
        constexpr char dir_chars[4] = {'^', '>', 'v', '<'};
        out.push_back(dir_chars[final_f]);
    }

    if (m.kind != MoveKind::Drop && m.src >= 0 && m.src < NUM_SQUARES) {
        bool need_src = false;
        if (m.kind == MoveKind::Rotate) {
            need_src = true;
        } else if (m.dst >= 0 && m.dst < NUM_SQUARES) {
            int ambiguous_count = 0;
            for (int sq = 0; sq < NUM_SQUARES; ++sq) {
                if (pos.height[sq] == 0) continue;
                const Piece top = pos.top_piece[sq];
                if (top.owner != moving_owner || top.type != ptype) continue;
                RawDestList raw = get_raw_destinations(pos, sq);
                for (int i = 0; i < raw.count; ++i) {
                    if (raw.sq[i] == m.dst) {
                        ++ambiguous_count;
                        break;
                    }
                }
            }
            bool rotate_conflict = pos.height[m.dst] > 0 &&
                                   pos.top_piece[m.dst].owner == moving_owner &&
                                   pos.top_piece[m.dst].type == ptype &&
                                   can_rotate(pos.top_piece[m.dst].type);
            if (ambiguous_count > 1 || rotate_conflict) {
                need_src = true;
            }
        }
        if (need_src) {
            out.push_back('.');
            out += sq_to_str(m.src);
        }
    }

    return out;
}

bool parse_canonical_notation(const Position& pos, std::string_view text_in, Move& out_move, std::string& err) {
    if (text_in.empty() || text_in.size() > 256) {
        err = "Invalid notation length";
        return false;
    }
    std::string text(text_in);
    // Normalize UTF-8 arrows if present
    for (const auto& [from, to] : {
        std::pair<std::string_view, std::string_view>{"\xCB\x84", "^"},
        std::pair<std::string_view, std::string_view>{"\xCB\x85", "v"}
    }) {
        size_t pos_idx = 0;
        while ((pos_idx = text.find(from, pos_idx)) != std::string::npos) {
            text.replace(pos_idx, from.size(), to);
            pos_idx += to.size();
        }
    }

    bool mate = false;
    if (!text.empty() && text.back() == '#') {
        mate = true;
        text.pop_back();
    }

    MoveList legal;
    generate_legal_moves(pos, legal);

    // First pass: exact canonical match
    std::vector<Move> exact_matches;
    for (int i = 0; i < legal.count; ++i) {
        const Move& m = legal.moves[i];
        if (format_canonical_notation(pos, m) == text) {
            if (mate) {
                Position copy = pos;
                UndoState u;
                apply_move(copy, m, u);
                MoveList child_legal;
                generate_legal_moves(copy, child_legal);
                if (!is_in_check(copy, static_cast<Side>(copy.side_to_move)) || child_legal.count != 0) {
                    continue;
                }
            }
            exact_matches.push_back(m);
        }
    }
    if (exact_matches.size() == 1) {
        out_move = exact_matches[0];
        return true;
    }

    // Second pass: compact / explicit .src matching (matching ryuugin::parse_move)
    size_t text_dot = text.find('.');
    std::string text_compact = (text_dot == std::string::npos) ? text : text.substr(0, text_dot);
    std::vector<Move> matches;
    for (int i = 0; i < legal.count; ++i) {
        const Move& m = legal.moves[i];
        std::string full = format_canonical_notation(pos, m);
        size_t dot = full.find('.');
        std::string compact = (dot == std::string::npos) ? full : full.substr(0, dot);
        bool matched = (text == full || text == compact);
        if (!matched && text_dot != std::string::npos && text_compact == compact && m.src >= 0) {
            if (sq_to_str(m.src) == text.substr(text_dot + 1)) {
                matched = true;
            }
        }
        if (matched) {
            if (mate) {
                Position copy = pos;
                UndoState u;
                apply_move(copy, m, u);
                MoveList child_legal;
                generate_legal_moves(copy, child_legal);
                if (!is_in_check(copy, static_cast<Side>(copy.side_to_move)) || child_legal.count != 0) {
                    continue;
                }
            }
            matches.push_back(m);
        }
    }

    if (matches.size() != 1) {
        err = matches.empty() ? "Illegal move notation" : "Ambiguous move notation";
        return false;
    }
    out_move = matches[0];
    return true;
}

} // namespace homei

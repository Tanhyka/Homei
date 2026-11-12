#include "lppn.h"
#include "zobrist.h"
#include <cctype>
#include <vector>

namespace homei {

namespace {

constexpr size_t MAX_LPPN_LENGTH = 8192;

std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' || s[i] == '\v' || s[i] == '\f')) {
            ++i;
        }
        if (i >= s.size()) break;
        size_t start = i;
        while (i < s.size() && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' || s[i] == '\v' || s[i] == '\f')) {
            ++i;
        }
        out.push_back(s.substr(start, i - start));
    }
    return out;
}

std::vector<std::string_view> split_char(std::string_view s, char delim) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(delim, start);
        if (pos == std::string_view::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

bool decode_pair(std::string_view pair, Piece& out_piece, std::string& err) {
    if (pair.size() != 2) {
        err = "Invalid LPPN piece token";
        return false;
    }
    char ch = pair[0];
    char fc = pair[1];
    if (fc < '0' || fc > '3') {
        err = "Invalid LPPN piece token";
        return false;
    }
    int pt = parse_piece_code_char(ch);
    if (pt < 0) {
        err = "Bad piece type in LPPN";
        return false;
    }
    uint8_t owner = (ch >= 'A' && ch <= 'Z') ? CAM : XANH;
    uint8_t facing = static_cast<uint8_t>(fc - '0');
    out_piece = Piece(static_cast<uint8_t>(pt), owner, facing);
    return true;
}

bool parse_hands_field(std::string_view text, bool is_reserve, Position& pos, int& total_pieces, std::string& err) {
    auto halves = split_char(text, '/');
    if (halves.size() != 2) {
        err = "LPPN hands missing CAM/XANH";
        return false;
    }
    for (int side = 0; side < 2; ++side) {
        std::string_view chars = halves[side];
        if (chars == "-") continue;
        if (chars.empty()) {
            err = "Empty hand token";
            return false;
        }
        bool seen_reserve[8]{};
        for (char ch : chars) {
            if (!(ch >= 'A' && ch <= 'Z')) {
                err = "Invalid hand symbol";
                return false;
            }
            int pt = parse_piece_code_char(ch);
            if (pt < 0) {
                err = "Bad piece type in LPPN";
                return false;
            }
            if (pt == VUONG) {
                err = "VUONG in hand";
                return false;
            }
            if (is_reserve) {
                if (pt != LONG && pt != PHUONG) {
                    err = "Invalid reserve type";
                    return false;
                }
                if (seen_reserve[pt]) {
                    err = "Duplicate reserve piece";
                    return false;
                }
                seen_reserve[pt] = true;
                pos.set_reserve_type(static_cast<Side>(side), static_cast<uint8_t>(pt), true);
            } else {
                if (pos.prisoners[side][pt] >= MAX_TOTAL_PIECES) {
                    err = "Over 40 pieces";
                    return false;
                }
                pos.prisoners[side][pt]++;
            }
            total_pieces++;
        }
    }
    return true;
}

void append_piece_token(const Piece& p, std::string& out) {
    char code = piece_code_char(p.type);
    if (p.owner == XANH) {
        code = static_cast<char>(code - 'A' + 'a');
    }
    out.push_back(code);
    out.push_back(static_cast<char>('0' + p.facing));
}

} // namespace

bool validate_position(Position& pos, bool strict, std::string& err) {
    int kings[2] = {0, 0};
    int total = 0;
    pos.king_sq[0] = 255;
    pos.king_sq[1] = 255;

    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        const Cell& c = pos.cell[sq];
        if (c.size > MAX_STACK_SIZE) {
            err = "Stack exceeds limit";
            return false;
        }
        if (c.size > 0) {
            uint8_t owner = c.stack[0].owner;
            for (uint8_t i = 0; i < c.size; ++i) {
                const Piece& p = c.stack[i];
                if (p.owner != owner) {
                    err = "Mixed-owner stack";
                    return false;
                }
                if (p.type == VUONG) {
                    if (c.size > 1) {
                        err = "VUONG stacked";
                        return false;
                    }
                    kings[owner]++;
                    pos.king_sq[owner] = static_cast<uint8_t>(sq);
                }
            }
            total += c.size;
        }
    }

    for (int side = 0; side < 2; ++side) {
        if (pos.reserve[side] & ~(RESERVE_LONG | RESERVE_PHUONG)) {
            err = "Invalid reserve type";
            return false;
        }
        if (pos.reserve[side] & RESERVE_LONG) total++;
        if (pos.reserve[side] & RESERVE_PHUONG) total++;

        if (pos.prisoners[side][VUONG] > 0) {
            err = "VUONG in hand";
            return false;
        }
        for (int pt = 0; pt < 8; ++pt) {
            total += pos.prisoners[side][pt];
        }
    }

    if (strict) {
        if (kings[CAM] != 1 || kings[XANH] != 1) {
            err = "Strict LPPN requires one VUONG per side";
            return false;
        }
        if (total > MAX_TOTAL_PIECES) {
            err = "Over 40 pieces";
            return false;
        }
    }
    return true;
}

bool parse_lppn(std::string_view text, Position& out_pos, int& out_ply, std::string& err, bool strict) {
    if (text.size() > MAX_LPPN_LENGTH) {
        err = "LPPN too long";
        return false;
    }
    auto fields = split_ws(text);
    if (fields.size() != 6 || fields[0] != "lppn1" || (fields[2] != "c" && fields[2] != "x")) {
        err = "Expected lppn1 BOARD TURN RESERVE PRISONERS PLY";
        return false;
    }

    std::string_view encoded_board = fields[1];
    std::string_view side_text = fields[2];
    std::string_view reserve_text = fields[3];
    std::string_view prisoners_text = fields[4];
    std::string_view ply_text = fields[5];

    if (ply_text.empty() || (ply_text.size() > 1 && ply_text[0] == '0') || ply_text.size() > 7) {
        err = "Invalid ply";
        return false;
    }
    int ply = 0;
    for (char ch : ply_text) {
        if (ch < '0' || ch > '9') {
            err = "Invalid ply";
            return false;
        }
        ply = ply * 10 + (ch - '0');
    }
    if (ply < 0 || ply > 1000000) {
        err = "Ply out of range";
        return false;
    }

    Position pos;
    pos.clear();
    pos.side_to_move = (side_text == "c") ? CAM : XANH;

    int total_pieces = 0;
    if (!parse_hands_field(reserve_text, true, pos, total_pieces, err)) return false;
    if (!parse_hands_field(prisoners_text, false, pos, total_pieces, err)) return false;

    auto rows = split_char(encoded_board, '/');
    if (rows.size() != BOARD_SIZE) {
        err = "Must have 9 rows";
        return false;
    }

    for (int r = 0; r < BOARD_SIZE; ++r) {
        std::string_view row = rows[r];
        int col = 0;
        size_t i = 0;
        while (i < row.size()) {
            char ch = row[i];
            if (ch >= '1' && ch <= '9') {
                col += (ch - '0');
                ++i;
            } else {
                if (col >= BOARD_SIZE) {
                    err = "Row too wide";
                    return false;
                }
                Cell& cell = pos.cell[make_sq(r, col)];
                if (ch == '[') {
                    size_t end = row.find(']', i + 1);
                    if (end == std::string_view::npos || (end - i - 1) < 4 || ((end - i - 1) % 2) != 0) {
                        err = "Malformed stack";
                        return false;
                    }
                    size_t num_pieces = (end - i - 1) / 2;
                    if (num_pieces > MAX_STACK_SIZE) {
                        err = "Stack exceeds limit";
                        return false;
                    }
                    for (size_t n = i + 1; n < end; n += 2) {
                        Piece p;
                        if (!decode_pair(row.substr(n, 2), p, err)) return false;
                        cell.stack[cell.size++] = p;
                    }
                    i = end + 1;
                } else {
                    if (i + 2 > row.size()) {
                        err = "Truncated piece";
                        return false;
                    }
                    Piece p;
                    if (!decode_pair(row.substr(i, 2), p, err)) return false;
                    cell.stack[cell.size++] = p;
                    i += 2;
                }
                col += 1;
            }
            if (col > BOARD_SIZE) {
                err = "Row exceeds 9 squares";
                return false;
            }
        }
        if (col != BOARD_SIZE) {
            err = "Row must contain 9 squares";
            return false;
        }
    }

    if (!validate_position(pos, strict, err)) return false;

    pos.zobrist = compute_zobrist(pos);
    out_pos = pos;
    out_ply = ply;
    return true;
}

bool format_lppn(const Position& pos, int ply, std::string& out_text, std::string& err, bool strict) {
    if (ply < 0 || ply > 1000000) {
        err = "Ply out of range";
        return false;
    }
    Position copy_pos = pos;
    if (!validate_position(copy_pos, strict, err)) return false;

    std::string res = "lppn1 ";
    for (int r = 0; r < BOARD_SIZE; ++r) {
        if (r > 0) res.push_back('/');
        int empty = 0;
        for (int c = 0; c < BOARD_SIZE; ++c) {
            const Cell& cell = pos.cell[make_sq(r, c)];
            if (cell.empty()) {
                empty++;
                continue;
            }
            if (empty > 0) {
                res.push_back(static_cast<char>('0' + empty));
                empty = 0;
            }
            if (cell.size == 1) {
                append_piece_token(cell.stack[0], res);
            } else {
                res.push_back('[');
                for (uint8_t i = 0; i < cell.size; ++i) {
                    append_piece_token(cell.stack[i], res);
                }
                res.push_back(']');
            }
        }
        if (empty > 0) {
            res.push_back(static_cast<char>('0' + empty));
        }
    }

    res.push_back(' ');
    res.push_back(pos.side_to_move == CAM ? 'c' : 'x');
    res.push_back(' ');

    // Reserve
    for (int s = 0; s < 2; ++s) {
        if (s == 1) res.push_back('/');
        std::string h;
        if (pos.reserve[s] & RESERVE_LONG) h.push_back('L');
        if (pos.reserve[s] & RESERVE_PHUONG) h.push_back('P');
        if (h.empty()) res.push_back('-');
        else res.append(h);
    }

    res.push_back(' ');

    // Prisoners (canonical order: V, T, S, K, X, L, P, B)
    for (int s = 0; s < 2; ++s) {
        if (s == 1) res.push_back('/');
        std::string h;
        for (int pt = 0; pt < 8; ++pt) {
            uint8_t cnt = pos.prisoners[s][pt];
            for (uint8_t k = 0; k < cnt; ++k) {
                h.push_back(piece_code_char(static_cast<uint8_t>(pt)));
            }
        }
        if (h.empty()) res.push_back('-');
        else res.append(h);
    }

    res.push_back(' ');
    res.append(std::to_string(ply));

    if (res.size() > MAX_LPPN_LENGTH) {
        err = "LPPN too long";
        return false;
    }
    out_text = std::move(res);
    return true;
}

void setup_initial_position(Position& pos) {
    pos.clear();
    constexpr uint8_t back_row[9] = {XA, KI, SI, TUONG, VUONG, TUONG, SI, KI, XA};
    for (int s = 0; s < 2; ++s) {
        Side side = static_cast<Side>(s);
        uint8_t f = initial_facing(side);
        int br = (side == CAM) ? 0 : 8;
        int pr = (side == CAM) ? 2 : 6;
        for (int c = 0; c < 9; ++c) {
            int sq_b = make_sq(br, c);
            pos.cell[sq_b].stack[0] = Piece(back_row[c], s, f);
            pos.cell[sq_b].size = 1;
            if (back_row[c] == VUONG) {
                pos.king_sq[s] = static_cast<uint8_t>(sq_b);
            }

            int sq_p = make_sq(pr, c);
            pos.cell[sq_p].stack[0] = Piece(BINH, s, f);
            pos.cell[sq_p].size = 1;
        }
        pos.reserve[s] = RESERVE_LONG | RESERVE_PHUONG;
    }
    pos.side_to_move = CAM;
    pos.zobrist = compute_zobrist(pos);
}

} // namespace homei

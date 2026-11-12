#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace homei {

constexpr int BOARD_SIZE = 9;
constexpr int NUM_SQUARES = 81;
constexpr int MAX_STACK_SIZE = 38;
constexpr int MAX_TOTAL_PIECES = 40;
constexpr int SCORE_MATE = 30000;
constexpr int SCORE_INFINITE = 32000;
constexpr int MAX_PLY = 128;

enum PieceType : uint8_t {
    VUONG  = 0, // V - King (Fixed)
    TUONG  = 1, // T - Gold General (Rotatable)
    SI     = 2, // S - Silver General (Rotatable)
    KI     = 3, // K - Knight-like jumper (Rotatable)
    XA     = 4, // X - Lance (Rotatable)
    LONG   = 5, // L - Rook (Fixed)
    PHUONG = 6, // P - Bishop (Fixed)
    BINH   = 7, // B - Pawn (Rotatable)
    PIECE_TYPE_COUNT = 8
};

enum Side : uint8_t {
    CAM  = 0, // Orange / First player (row 0, facing 2)
    XANH = 1, // Green / Second player (row 8, facing 0)
    SIDE_COUNT = 2
};

constexpr inline Side other_side(Side s) {
    return s == CAM ? XANH : CAM;
}

constexpr inline uint8_t initial_facing(Side s) {
    return s == CAM ? 2 : 0;
}

constexpr inline bool can_rotate(uint8_t pt) {
    return pt != VUONG && pt != LONG && pt != PHUONG;
}

constexpr inline int make_sq(int r, int c) {
    return r * BOARD_SIZE + c;
}

constexpr inline int sq_row(int sq) {
    return sq / BOARD_SIZE;
}

constexpr inline int sq_col(int sq) {
    return sq % BOARD_SIZE;
}

constexpr inline bool in_bounds(int r, int c) {
    return r >= 0 && r < BOARD_SIZE && c >= 0 && c < BOARD_SIZE;
}

constexpr inline char piece_code_char(uint8_t pt) {
    constexpr char codes[8] = {'V', 'T', 'S', 'K', 'X', 'L', 'P', 'B'};
    return (pt < 8) ? codes[pt] : '?';
}

inline const char* piece_type_name(uint8_t pt) {
    static const char* const names[8] = {
        "VUONG", "TUONG", "SI", "KI", "XA", "LONG", "PHUONG", "BINH"
    };
    return (pt < 8) ? names[pt] : "UNKNOWN";
}

inline int parse_piece_type_name(const std::string& s) {
    for (int i = 0; i < 8; ++i) {
        if (s == piece_type_name(static_cast<uint8_t>(i))) return i;
    }
    return -1;
}

inline int parse_piece_code_char(char ch) {
    switch (ch) {
        case 'V': case 'v': return VUONG;
        case 'T': case 't': return TUONG;
        case 'S': case 's': return SI;
        case 'K': case 'k': return KI;
        case 'X': case 'x': return XA;
        case 'L': case 'l': return LONG;
        case 'P': case 'p': return PHUONG;
        case 'B': case 'b': return BINH;
        default: return -1;
    }
}

inline const char* side_name(Side s) {
    return s == CAM ? "CAM" : "XANH";
}

inline std::string sq_to_str(int sq) {
    if (sq < 0 || sq >= NUM_SQUARES) return "";
    char buf[3];
    buf[0] = static_cast<char>('a' + sq_row(sq));
    buf[1] = static_cast<char>('9' - sq_col(sq));
    buf[2] = '\0';
    return std::string(buf, 2);
}

inline int str_to_sq(const std::string& s) {
    if (s.size() != 2) return -1;
    int r = s[0] - 'a';
    int c = '9' - s[1];
    if (!in_bounds(r, c)) return -1;
    return make_sq(r, c);
}

struct Piece {
    uint8_t type   : 3;
    uint8_t owner  : 1;
    uint8_t facing : 2;

    constexpr Piece() : type(0), owner(0), facing(0) {}
    constexpr Piece(uint8_t t, uint8_t o, uint8_t f)
        : type(t & 7), owner(o & 1), facing(f & 3) {}

    constexpr uint8_t code64() const {
        return static_cast<uint8_t>(((owner * 8 + type) * 4) + facing);
    }

    constexpr bool operator==(const Piece& o) const {
        return type == o.type && owner == o.owner && facing == o.facing;
    }
    constexpr bool operator!=(const Piece& o) const {
        return !(*this == o);
    }
};
static_assert(sizeof(Piece) == 1, "Piece must be 1 byte");

struct Cell {
    uint8_t size = 0;
    Piece stack[MAX_STACK_SIZE];

    constexpr bool empty() const { return size == 0; }
    constexpr const Piece& top() const { return stack[size - 1]; }
    constexpr Piece& top() { return stack[size - 1]; }
    constexpr Side owner() const { return static_cast<Side>(stack[size - 1].owner); }

    bool contains_king() const {
        for (uint8_t i = 0; i < size; ++i) {
            if (stack[i].type == VUONG) return true;
        }
        return false;
    }
};

// Reserve bitmask constants
constexpr uint8_t RESERVE_LONG   = 1 << 0;
constexpr uint8_t RESERVE_PHUONG = 1 << 1;

struct Position {
    Cell cell[NUM_SQUARES];
    uint8_t prisoners[2][8]{};
    uint8_t reserve[2]{};      // bit0 = LONG, bit1 = PHUONG
    uint8_t king_sq[2]{255, 255};
    uint8_t side_to_move = CAM;
    uint64_t zobrist = 0;

    void clear() {
        for (int i = 0; i < NUM_SQUARES; ++i) {
            cell[i].size = 0;
        }
        std::memset(prisoners, 0, sizeof(prisoners));
        reserve[0] = 0;
        reserve[1] = 0;
        king_sq[0] = 255;
        king_sq[1] = 255;
        side_to_move = CAM;
        zobrist = 0;
    }

    bool has_reserve(Side s) const {
        return reserve[s] != 0;
    }

    bool has_reserve_type(Side s, uint8_t pt) const {
        if (pt == LONG) return (reserve[s] & RESERVE_LONG) != 0;
        if (pt == PHUONG) return (reserve[s] & RESERVE_PHUONG) != 0;
        return false;
    }

    void set_reserve_type(Side s, uint8_t pt, bool present) {
        uint8_t mask = (pt == LONG) ? RESERVE_LONG : (pt == PHUONG ? RESERVE_PHUONG : 0);
        if (present) reserve[s] |= mask;
        else reserve[s] &= static_cast<uint8_t>(~mask);
    }

    bool StructuralEqual(const Position& o) const {
        if (side_to_move != o.side_to_move) return false;
        if (reserve[0] != o.reserve[0] || reserve[1] != o.reserve[1]) return false;
        if (std::memcmp(prisoners, o.prisoners, sizeof(prisoners)) != 0) return false;
        for (int i = 0; i < NUM_SQUARES; ++i) {
            if (cell[i].size != o.cell[i].size) return false;
            for (uint8_t j = 0; j < cell[i].size; ++j) {
                if (cell[i].stack[j] != o.cell[i].stack[j]) return false;
            }
        }
        return true;
    }
};

enum class MoveKind : uint8_t {
    Single = 0,
    Team   = 1,
    Rotate = 2,
    Drop   = 3
};

struct Move {
    MoveKind kind = MoveKind::Single;
    int8_t src = -1;          // 0..80, or -1 for Drop
    int8_t dst = -1;          // 0..80, or -1 for Rotate
    uint8_t ptype = 0;        // PieceType 0..7
    int8_t facing = -1;       // 0..3 for Drop, -1 otherwise
    uint8_t rotate_after = 0; // 0, 1, 3

    constexpr bool is_none() const {
        return src == -1 && dst == -1;
    }

    static constexpr Move none() {
        return Move{MoveKind::Single, -1, -1, 0, -1, 0};
    }

    constexpr bool operator==(const Move& o) const {
        return kind == o.kind && src == o.src && dst == o.dst &&
               ptype == o.ptype && facing == o.facing && rotate_after == o.rotate_after;
    }
    constexpr bool operator!=(const Move& o) const {
        return !(*this == o);
    }
};

struct UndoState {
    Piece captured[MAX_STACK_SIZE];
    uint8_t captured_size = 0;
    uint8_t prev_reserve[2]{};
    uint8_t prev_king_sq[2]{};
    uint8_t prev_top_facing = 0;
    uint64_t prev_zobrist = 0;
};

struct RepetitionEntry {
    Position position;
    // Also preserve exact prisoner list order for GUI _position_identity_py if needed,
    // while zobrist is used for fast table lookup.
    uint64_t hash = 0;
    int count = 0;
};

enum class GameStatus : uint8_t {
    Ongoing = 0,
    Terminal = 1
};

struct GameResult {
    GameStatus status = GameStatus::Ongoing;
    std::string reason; // "repetition3", "checkmate", "pat", or ""
    int winner = -1;    // 0 = CAM, 1 = XANH, -1 = none
    int loser = -1;     // 0 = CAM, 1 = XANH, -1 = none
};

struct GameState {
    Position position;
    int ply = 0;
    bool repetition_complete = true;
    std::vector<RepetitionEntry> history;
    GameResult result;
    std::string notation;
    bool has_notation = false;
};

} // namespace homei

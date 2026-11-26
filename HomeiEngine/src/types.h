#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace homei {

constexpr int BOARD_SIZE = 9;
constexpr int NUM_SQUARES = 81;
constexpr int MAX_STACK_SIZE = 38;
constexpr int MAX_TOTAL_PIECES = 40;
constexpr int MAX_BURIED_POOL = 48;
constexpr uint8_t BURIED_NONE = 255;
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

// Reserve bitmask constants
constexpr uint8_t RESERVE_LONG   = 1 << 0;
constexpr uint8_t RESERVE_PHUONG = 1 << 1;

constexpr uint64_t BB_HI_MASK = (1ULL << (NUM_SQUARES - 64)) - 1ULL; // 17 valid bits for squares 64..80

struct Bitboard {
    uint64_t lo = 0;
    uint64_t hi = 0;

    constexpr Bitboard() = default;
    constexpr Bitboard(uint64_t l, uint64_t h) : lo(l), hi(h & BB_HI_MASK) {}

    static constexpr Bitboard from_sq(int sq) {
        if (sq < 64) return Bitboard(1ULL << sq, 0ULL);
        return Bitboard(0ULL, 1ULL << (sq - 64));
    }

    constexpr void clear_all() { lo = 0; hi = 0; }
    constexpr void set_bit(int sq) {
        if (sq < 64) lo |= (1ULL << sq);
        else hi |= (1ULL << (sq - 64));
    }
    constexpr void clear_bit(int sq) {
        if (sq < 64) lo &= ~(1ULL << sq);
        else hi &= ~(1ULL << (sq - 64));
    }
    constexpr void toggle_bit(int sq) {
        if (sq < 64) lo ^= (1ULL << sq);
        else hi ^= (1ULL << (sq - 64));
    }
    constexpr bool test_bit(int sq) const {
        if (sq < 64) return ((lo >> sq) & 1ULL) != 0;
        return ((hi >> (sq - 64)) & 1ULL) != 0;
    }
    constexpr bool any() const { return (lo | hi) != 0ULL; }
    constexpr bool empty() const { return (lo | hi) == 0ULL; }
    constexpr bool more_than_one() const {
        return ((lo != 0ULL) && (hi != 0ULL)) || ((lo & (lo - 1ULL)) != 0ULL) || ((hi & (hi - 1ULL)) != 0ULL);
    }

    inline int popcount() const {
        return std::popcount(lo) + std::popcount(hi);
    }
    inline int lsb() const {
        return (lo != 0ULL) ? std::countr_zero(lo) : (64 + std::countr_zero(hi));
    }
    inline int msb() const {
        return (hi != 0ULL) ? (64 + 63 - std::countl_zero(hi)) : (63 - std::countl_zero(lo));
    }
    inline int pop_lsb() {
        if (lo != 0ULL) {
            int s = std::countr_zero(lo);
            lo &= (lo - 1ULL);
            return s;
        }
        int s = 64 + std::countr_zero(hi);
        hi &= (hi - 1ULL);
        return s;
    }

    constexpr Bitboard operator|(const Bitboard& o) const { return Bitboard(lo | o.lo, hi | o.hi); }
    constexpr Bitboard operator&(const Bitboard& o) const { return Bitboard(lo & o.lo, hi & o.hi); }
    constexpr Bitboard operator^(const Bitboard& o) const { return Bitboard(lo ^ o.lo, hi ^ o.hi); }
    constexpr Bitboard operator~() const { return Bitboard(~lo, (~hi) & BB_HI_MASK); }
    constexpr Bitboard& operator|=(const Bitboard& o) { lo |= o.lo; hi |= o.hi; return *this; }
    constexpr Bitboard& operator&=(const Bitboard& o) { lo &= o.lo; hi &= o.hi; return *this; }
    constexpr Bitboard& operator^=(const Bitboard& o) { lo ^= o.lo; hi ^= o.hi; return *this; }
    constexpr bool operator==(const Bitboard& o) const { return lo == o.lo && hi == o.hi; }
    constexpr bool operator!=(const Bitboard& o) const { return !(*this == o); }
};

// Compact Homei v4.3 Position (~584 B total, hot region first ~392 B; down from 3,336 B in v4.2).
// Hot fields (top_piece, height, bitboards, keys, hands) live in the first ~6 cache lines.
// Cold buried stack pool (buried_head, buried_piece, buried_next) is only touched when height[sq] >= 2.
struct Position {
    // --- Hot cache lines ---
    Piece top_piece[NUM_SQUARES]{};
    uint8_t height[NUM_SQUARES]{};
    uint8_t prisoners[2][8]{};
    uint8_t reserve[2]{};      // bit0 = LONG, bit1 = PHUONG
    uint8_t king_sq[2]{255, 255};
    uint8_t side_to_move = CAM;
    uint8_t buried_free_head = 0;
    uint8_t buried_count = 0;
    uint8_t pad0_[5]{};

    uint64_t board_key = 0;    // XOR of board pieces + side_to_move (bit 0 == side_to_move)
    uint64_t hand_key = 0;     // Additive hash of prisoners + reserve (bit 0 == 0)
    uint64_t zobrist = 0;      // board_key ^ hand_key (bit 0 == side_to_move)

    Bitboard bb_occ;
    Bitboard bb_side[2];
    Bitboard bb_type[PIECE_TYPE_COUNT];
    Bitboard bb_stack;         // squares with height >= 2

    // --- Cold tail (only touched for stacked cells with height >= 2) ---
    uint8_t buried_head[NUM_SQUARES]{};
    Piece buried_piece[MAX_BURIED_POOL]{};
    uint8_t buried_next[MAX_BURIED_POOL]{};

    Position() { clear(); }
    Position(const Position&) = default;
    Position& operator=(const Position&) = default;

    // Bitboard helpers
    inline Bitboard pieces(Side s, uint8_t pt) const { return bb_side[s] & bb_type[pt]; }
    inline Bitboard bb_long(Side s) const { return bb_side[s] & bb_type[LONG]; }
    inline Bitboard bb_phuong(Side s) const { return bb_side[s] & bb_type[PHUONG]; }
    inline Bitboard bb_xa(Side s) const { return bb_side[s] & bb_type[XA]; }
    inline Bitboard bb_sliders() const { return bb_type[LONG] | bb_type[PHUONG] | bb_type[XA]; }
    inline Bitboard bb_step_pieces(Side s) const { return bb_side[s] & ~bb_sliders(); }

    inline void bb_add_top(Side s, uint8_t pt, int sq) {
        bb_occ.set_bit(sq);
        bb_side[s].set_bit(sq);
        bb_type[pt].set_bit(sq);
    }

    inline void bb_remove_top(Side s, uint8_t pt, int sq) {
        bb_occ.clear_bit(sq);
        bb_side[s].clear_bit(sq);
        bb_type[pt].clear_bit(sq);
    }

    // Fast O(1) cell accessors
    inline bool cell_empty(int sq) const { return height[sq] == 0; }
    inline uint8_t cell_size(int sq) const { return height[sq]; }
    inline const Piece& cell_top(int sq) const { return top_piece[sq]; }
    inline Piece& cell_top(int sq) { return top_piece[sq]; }
    inline Side cell_owner(int sq) const { return static_cast<Side>(top_piece[sq].owner); }

    inline Piece cell_second(int sq) const {
        return buried_piece[buried_head[sq]];
    }

    inline bool cell_contains_king(int sq) const {
        uint8_t h = height[sq];
        if (h == 0) return false;
        if (top_piece[sq].type == VUONG) return true;
        if (h == 1) return false;
        for (uint8_t idx = buried_head[sq]; idx != BURIED_NONE; idx = buried_next[idx]) {
            if (buried_piece[idx].type == VUONG) return true;
        }
        return false;
    }

    // Copies stack at `sq` into `out[0..height-1]` in bottom-to-top order. Returns height.
    inline int cell_copy_stack(int sq, Piece out[MAX_STACK_SIZE]) const {
        uint8_t h = height[sq];
        if (h == 0) return 0;
        out[h - 1] = top_piece[sq];
        int pos = static_cast<int>(h) - 2;
        for (uint8_t idx = buried_head[sq]; idx != BURIED_NONE && pos >= 0; idx = buried_next[idx]) {
            out[pos--] = buried_piece[idx];
        }
        return static_cast<int>(h);
    }

    inline uint8_t alloc_buried(Piece p, uint8_t next_idx) {
        uint8_t idx = buried_free_head;
        if (idx == BURIED_NONE) return BURIED_NONE;
        buried_free_head = buried_next[idx];
        buried_piece[idx] = p;
        buried_next[idx] = next_idx;
        ++buried_count;
        return idx;
    }

    inline void free_buried(uint8_t idx) {
        buried_next[idx] = buried_free_head;
        buried_free_head = idx;
        --buried_count;
    }

    inline void cell_push(int sq, Piece p) {
        uint8_t h = height[sq];
        if (h == 0) {
            top_piece[sq] = p;
            height[sq] = 1;
            buried_head[sq] = BURIED_NONE;
        } else {
            uint8_t idx = alloc_buried(top_piece[sq], buried_head[sq]);
            buried_head[sq] = idx;
            top_piece[sq] = p;
            height[sq] = static_cast<uint8_t>(h + 1);
            if (h == 1) bb_stack.set_bit(sq);
        }
    }

    inline Piece cell_pop(int sq) {
        Piece p = top_piece[sq];
        uint8_t h = --height[sq];
        if (h == 0) {
            buried_head[sq] = BURIED_NONE;
        } else {
            uint8_t idx = buried_head[sq];
            top_piece[sq] = buried_piece[idx];
            buried_head[sq] = buried_next[idx];
            free_buried(idx);
            if (h == 1) bb_stack.clear_bit(sq);
        }
        return p;
    }

    inline void cell_clear(int sq) {
        uint8_t h = height[sq];
        if (h == 0) return;
        uint8_t idx = buried_head[sq];
        while (idx != BURIED_NONE) {
            uint8_t nxt = buried_next[idx];
            free_buried(idx);
            idx = nxt;
        }
        height[sq] = 0;
        buried_head[sq] = BURIED_NONE;
        if (h >= 2) bb_stack.clear_bit(sq);
    }

    inline void sync_bitboards() {
        bb_occ.clear_all();
        bb_side[0].clear_all();
        bb_side[1].clear_all();
        for (int pt = 0; pt < PIECE_TYPE_COUNT; ++pt) bb_type[pt].clear_all();
        bb_stack.clear_all();
        for (int sq = 0; sq < NUM_SQUARES; ++sq) {
            uint8_t h = height[sq];
            if (h > 0) {
                const Piece& top = top_piece[sq];
                bb_add_top(static_cast<Side>(top.owner), top.type, sq);
                if (h >= 2) bb_stack.set_bit(sq);
            }
        }
    }

    void clear() {
        std::memset(height, 0, sizeof(height));
        std::memset(top_piece, 0, sizeof(top_piece));
        std::memset(prisoners, 0, sizeof(prisoners));
        reserve[0] = 0;
        reserve[1] = 0;
        king_sq[0] = 255;
        king_sq[1] = 255;
        side_to_move = CAM;
        board_key = 0;
        hand_key = 0;
        zobrist = 0;
        bb_occ.clear_all();
        bb_side[0].clear_all();
        bb_side[1].clear_all();
        for (int pt = 0; pt < PIECE_TYPE_COUNT; ++pt) bb_type[pt].clear_all();
        bb_stack.clear_all();
        std::memset(buried_head, BURIED_NONE, sizeof(buried_head));
        for (uint8_t i = 0; i < MAX_BURIED_POOL - 1; ++i) {
            buried_next[i] = static_cast<uint8_t>(i + 1);
        }
        buried_next[MAX_BURIED_POOL - 1] = BURIED_NONE;
        buried_free_head = 0;
        buried_count = 0;
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
        if (std::memcmp(height, o.height, sizeof(height)) != 0) return false;
        for (int sq = 0; sq < NUM_SQUARES; ++sq) {
            uint8_t h = height[sq];
            if (h == 0) continue;
            if (top_piece[sq] != o.top_piece[sq]) return false;
            if (h >= 2) {
                uint8_t i1 = buried_head[sq];
                uint8_t i2 = o.buried_head[sq];
                while (i1 != BURIED_NONE && i2 != BURIED_NONE) {
                    if (buried_piece[i1] != o.buried_piece[i2]) return false;
                    i1 = buried_next[i1];
                    i2 = o.buried_next[i2];
                }
                if (i1 != i2) return false;
            }
        }
        return true;
    }
};
static_assert(std::is_trivially_copyable_v<Position>, "Position must be trivially copyable for fast memcpy");
static_assert(sizeof(Position) <= 640, "Position must stay compact (~584 B vs 3336 B in v4.2)");

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
    uint64_t prev_board_key = 0;
    uint64_t prev_hand_key = 0;
    uint64_t prev_zobrist = 0;
};

struct RepetitionEntry {
    Position position;
    uint64_t hash = 0;
    int count = 0;
};

enum class GameStatus : uint8_t {
    Ongoing = 0,
    Terminal = 1
};

struct GameResult {
    GameStatus status = GameStatus::Ongoing;
    std::string reason; // "repetition3", "repetition3_draw", "checkmate", "pat", or ""
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

// One-time global table initializer (called at startup + static AutoInit; zero atomic checks on hot paths).
void init_homei_tables();

} // namespace homei

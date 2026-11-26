#pragma once

#include "types.h"
#include <vector>

namespace homei {

constexpr int MAX_MOVES = 4096;

struct MoveList {
    static constexpr int MAX_MOVES = homei::MAX_MOVES;
    Move moves[MAX_MOVES];
    int count = 0;

    void clear() { count = 0; }
    void push(const Move& m) {
        if (count < MAX_MOVES) moves[count++] = m;
    }
    void add(const Move& m) { push(m); }
    const Move* begin() const { return moves; }
    const Move* end() const { return moves + count; }
    Move* begin() { return moves; }
    Move* end() { return moves + count; }
};

// Populates `out_dsts` with raw destination squares from `src_sq` in exact rules.py order.
int get_raw_destinations(const Position& pos, int src_sq, int out_dsts[32]);

struct RawDestList {
    enum Kind : uint8_t { Empty = 0, Own = 1, Capture = 2 };
    int sq[32]{};
    Kind kind[32]{};
    int count = 0;
};

inline RawDestList get_raw_destinations(const Position& pos, int src_sq) {
    RawDestList out;
    if (src_sq < 0 || src_sq >= NUM_SQUARES || pos.height[src_sq] == 0) return out;
    Side owner = pos.cell_owner(src_sq);
    out.count = get_raw_destinations(pos, src_sq, out.sq);
    for (int i = 0; i < out.count; ++i) {
        int dsq = out.sq[i];
        if (pos.height[dsq] == 0) out.kind[i] = RawDestList::Empty;
        else if (pos.cell_owner(dsq) == owner) out.kind[i] = RawDestList::Own;
        else out.kind[i] = RawDestList::Capture;
    }
    return out;
}

// Generates pseudo-legal moves in the exact order of rules.generate_moves.
void generate_pseudo_moves(const Position& pos, MoveList& out);

// Applies move `m` to `pos`, saving undo state in `u` and updating split Zobrist incrementally.
void apply_move(Position& pos, const Move& m, UndoState& u);

// Reverts move `m` on `pos` using `u`.
void undo_move(Position& pos, const Move& m, const UndoState& u);

// Generates strictly legal moves (zero apply_move/undo_move using pin blockers & checkersBB).
void generate_legal_moves(const Position& pos, MoveList& out);
std::vector<Move> generate_legal_moves(const Position& pos);

// Generates capture moves for quiescence & staged search.
void generate_capture_moves(const Position& pos, MoveList& out);

// Generates pseudo-legal quiet moves and drops (excluding captures).
void generate_quiet_and_drop_moves(const Position& pos, MoveList& out);

struct QuietGenFilter {
    bool skip_far_noncheck_drops = false;
    int max_plain_drop_facings = 4;
};

void generate_quiet_and_drop_moves(const Position& pos, MoveList& out, const QuietGenFilter& filter);

// Fast O(1) pseudo-legality check for a candidate move (TT move, killer, countermove).
bool is_pseudo_legal_move(const Position& pos, const Move& m);

// Zero-apply_move bitboard king-safety test for a pseudo-legal move `m`.
bool check_move_leaves_king_safe(const Position& pos, const Move& m, bool currently_in_check);

// Ultra-fast O(1) legality check when `!currently_in_check` and `pin_blockers` is precomputed for the node.
bool check_move_leaves_king_safe_pinned(const Position& pos, const Move& m, const Bitboard& pin_blockers);

// Combined O(1) pseudo-legality + king-safety check.
bool is_legal_move_fast(const Position& pos, const Move& m, bool currently_in_check);

// Perft node count using legal moves (without repetition/terminal adjudication cutoff).
uint64_t perft(Position& pos, int depth);

} // namespace homei

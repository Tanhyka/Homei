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
// Returns number of destinations.
int get_raw_destinations(const Position& pos, int src_sq, int out_dsts[32]);

struct RawDestList {
    enum Kind : uint8_t { Empty = 0, Own = 1, Capture = 2 };
    int sq[32]{};
    Kind kind[32]{};
    int count = 0;
};

inline RawDestList get_raw_destinations(const Position& pos, int src_sq) {
    RawDestList out;
    if (src_sq < 0 || src_sq >= NUM_SQUARES || pos.cell[src_sq].empty()) return out;
    Side owner = pos.cell[src_sq].owner();
    out.count = get_raw_destinations(pos, src_sq, out.sq);
    for (int i = 0; i < out.count; ++i) {
        int dsq = out.sq[i];
        if (pos.cell[dsq].empty()) out.kind[i] = RawDestList::Empty;
        else if (pos.cell[dsq].owner() == owner) out.kind[i] = RawDestList::Own;
        else out.kind[i] = RawDestList::Capture;
    }
    return out;
}

// Generates pseudo-legal moves in the exact order of rules.generate_moves.
void generate_pseudo_moves(const Position& pos, MoveList& out);

// Applies move `m` to `pos`, saving undo state in `u` and updating `pos.zobrist` incrementally.
void apply_move(Position& pos, const Move& m, UndoState& u);

// Reverts move `m` on `pos` using `u`.
void undo_move(Position& pos, const Move& m, const UndoState& u);

// Generates strictly legal moves (filtering out moves that leave mover's VUONG in check).
void generate_legal_moves(Position& pos, MoveList& out);
inline void generate_legal_moves(const Position& pos, MoveList& out) {
    Position copy = pos;
    generate_legal_moves(copy, out);
}
std::vector<Move> generate_legal_moves(const Position& pos);

// Generates capture moves (and optionally checks/drops) for quiescence search.
void generate_capture_moves(const Position& pos, MoveList& out);

// Perft node count using legal moves (without repetition/terminal adjudication cutoff, per spec 3.3).
uint64_t perft(Position& pos, int depth);

} // namespace homei

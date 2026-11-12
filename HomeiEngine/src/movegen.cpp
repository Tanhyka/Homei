#include "movegen.h"
#include "attack.h"
#include "geometry.h"
#include "zobrist.h"

namespace homei {

namespace {

inline uint8_t canonical_code64(const Piece& p) {
    uint8_t f = can_rotate(p.type) ? p.facing : initial_facing(static_cast<Side>(p.owner));
    return static_cast<uint8_t>(((p.owner * 8 + p.type) * 4) + f);
}

} // namespace

int get_raw_destinations(const Position& pos, int src_sq, int out_dsts[32]) {
    if (src_sq < 0 || src_sq >= NUM_SQUARES) return 0;
    const Cell& sq = pos.cell[src_sq];
    if (sq.empty()) return 0;

    const Piece& top = sq.top();
    const auto& mt = get_piece_moves(top.type, top.facing);
    int r = sq_row(src_sq);
    int c = sq_col(src_sq);
    int count = 0;

    for (uint8_t i = 0; i < mt.count; ++i) {
        int dr = mt.vecs[i].dr;
        int dc = mt.vecs[i].dc;
        if (!mt.vecs[i].sliding) {
            int r2 = r + dr;
            int c2 = c + dc;
            if (in_bounds(r2, c2)) {
                out_dsts[count++] = make_sq(r2, c2);
            }
        } else {
            int r2 = r + dr;
            int c2 = c + dc;
            while (in_bounds(r2, c2)) {
                int dsq = make_sq(r2, c2);
                out_dsts[count++] = dsq;
                if (!pos.cell[dsq].empty()) break;
                r2 += dr;
                c2 += dc;
            }
        }
    }
    return count;
}

void generate_pseudo_moves(const Position& pos, MoveList& out) {
    out.clear();
    Side side = static_cast<Side>(pos.side_to_move);
    uint8_t base_f = initial_facing(side);

    // 1. Reserve phase: if reserve[side] is non-empty, only reserve drops are allowed
    if (pos.reserve[side] != 0) {
        int drop_row = (side == CAM) ? 1 : 7;
        for (uint8_t pt : {LONG, PHUONG}) {
            if (!pos.has_reserve_type(side, pt)) continue;
            for (int c = 0; c < BOARD_SIZE; ++c) {
                int dsq = make_sq(drop_row, c);
                const Cell& dest = pos.cell[dsq];
                if (dest.empty() || (dest.owner() == side && dest.top().type != VUONG && dest.size < MAX_STACK_SIZE)) {
                    if (can_rotate(pt)) {
                        for (int8_t f = 0; f < 4; ++f) {
                            out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, f, 0});
                        }
                    } else {
                        out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, static_cast<int8_t>(base_f), 0});
                    }
                }
            }
        }
        return;
    }

    // 2. Main phase: prisoner drops first, ordered by PieceType (V, T, S, K, X, L, P, B)
    for (uint8_t pt = 0; pt < 8; ++pt) {
        if (pos.prisoners[side][pt] == 0) continue;
        bool rot = can_rotate(pt);
        for (int dsq = 0; dsq < NUM_SQUARES; ++dsq) {
            const Cell& dest = pos.cell[dsq];
            if (!dest.empty()) {
                if (dest.owner() != side || dest.contains_king() || dest.size >= MAX_STACK_SIZE) {
                    continue;
                }
            }
            if (rot) {
                for (int8_t f = 0; f < 4; ++f) {
                    out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, f, 0});
                }
            } else {
                out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, static_cast<int8_t>(base_f), 0});
            }
        }
    }

    // 3. Board moves: iterate squares 0..80 (r = 0..8, c = 0..8)
    int dsts[32];
    constexpr uint8_t rots_rotatable[3] = {0, 1, 3};
    constexpr uint8_t rots_fixed[1] = {0};

    for (int ssq = 0; ssq < NUM_SQUARES; ++ssq) {
        const Cell& src_cell = pos.cell[ssq];
        if (src_cell.empty() || src_cell.owner() != side) continue;

        const Piece& top = src_cell.top();
        bool is_stack = (src_cell.size > 1);
        bool src_has_king = src_cell.contains_king();
        bool top_rot = can_rotate(top.type);
        const uint8_t* rots = top_rot ? rots_rotatable : rots_fixed;
        int num_rots = top_rot ? 3 : 1;

        int num_dsts = get_raw_destinations(pos, ssq, dsts);
        for (int di = 0; di < num_dsts; ++di) {
            int dsq = dsts[di];
            const Cell& dest_cell = pos.cell[dsq];
            bool is_own = (!dest_cell.empty() && dest_cell.owner() == side);
            bool is_capture = (!dest_cell.empty() && !is_own);

            if (is_capture && dest_cell.contains_king()) {
                continue;
            }

            bool can_single = !is_own;
            if (is_own) {
                can_single = (!is_stack && top.type != VUONG && !dest_cell.contains_king() && dest_cell.size < MAX_STACK_SIZE);
            }
            bool can_team = (is_stack && !is_own && !src_has_king);

            for (int ri = 0; ri < num_rots; ++ri) {
                uint8_t rot = rots[ri];
                if (can_single) {
                    out.push(Move{MoveKind::Single, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rot});
                }
                if (can_team) {
                    out.push(Move{MoveKind::Team, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rot});
                }
            }
        }

        if (top_rot) {
            out.push(Move{MoveKind::Rotate, static_cast<int8_t>(ssq), -1, top.type, -1, 1});
            out.push(Move{MoveKind::Rotate, static_cast<int8_t>(ssq), -1, top.type, -1, 3});
        }
    }
}

void generate_capture_moves(const Position& pos, MoveList& out) {
    out.clear();
    Side side = static_cast<Side>(pos.side_to_move);
    if (pos.reserve[side] != 0) return;

    int dsts[32];
    constexpr uint8_t rots_rotatable[3] = {0, 1, 3};
    constexpr uint8_t rots_fixed[1] = {0};

    for (int ssq = 0; ssq < NUM_SQUARES; ++ssq) {
        const Cell& src_cell = pos.cell[ssq];
        if (src_cell.empty() || src_cell.owner() != side) continue;

        const Piece& top = src_cell.top();
        bool is_stack = (src_cell.size > 1);
        bool src_has_king = src_cell.contains_king();
        bool top_rot = can_rotate(top.type);
        const uint8_t* rots = top_rot ? rots_rotatable : rots_fixed;
        int num_rots = top_rot ? 3 : 1;

        int num_dsts = get_raw_destinations(pos, ssq, dsts);
        for (int di = 0; di < num_dsts; ++di) {
            int dsq = dsts[di];
            const Cell& dest_cell = pos.cell[dsq];
            if (dest_cell.empty() || dest_cell.owner() == side) continue;
            if (dest_cell.contains_king()) continue;

            bool can_team = (is_stack && !src_has_king);
            for (int ri = 0; ri < num_rots; ++ri) {
                uint8_t rot = rots[ri];
                out.push(Move{MoveKind::Single, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rot});
                if (can_team) {
                    out.push(Move{MoveKind::Team, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rot});
                }
            }
        }
    }
}

void apply_move(Position& pos, const Move& m, UndoState& u) {
    Side side = static_cast<Side>(pos.side_to_move);
    u.prev_reserve[0] = pos.reserve[0];
    u.prev_reserve[1] = pos.reserve[1];
    u.prev_king_sq[0] = pos.king_sq[0];
    u.prev_king_sq[1] = pos.king_sq[1];
    u.prev_zobrist = pos.zobrist;
    u.captured_size = 0;

    uint64_t h = pos.zobrist ^ zobrist_side;

    if (m.kind == MoveKind::Drop) {
        int dsq = m.dst;
        Cell& dest = pos.cell[dsq];
        uint8_t pt = m.ptype;
        uint8_t f = static_cast<uint8_t>((m.facing >= 0 ? m.facing : 0) & 3);
        Piece dropped(pt, side, f);

        if (pos.has_reserve_type(side, pt)) {
            uint8_t old_mask = pos.reserve[side] & 3;
            pos.set_reserve_type(side, pt, false);
            uint8_t new_mask = pos.reserve[side] & 3;
            h ^= zobrist_reserve[side][old_mask] ^ zobrist_reserve[side][new_mask];
        } else {
            uint8_t old_cnt = pos.prisoners[side][pt];
            pos.prisoners[side][pt] = old_cnt - 1;
            h ^= zobrist_prisoner[side][pt][old_cnt] ^ zobrist_prisoner[side][pt][old_cnt - 1];
        }

        h ^= zobrist_cell[dest.size][dsq][canonical_code64(dropped)];
        dest.stack[dest.size++] = dropped;
        pos.side_to_move = other_side(side);
        pos.zobrist = h;
        return;
    }

    int ssq = m.src;
    Cell& src = pos.cell[ssq];
    u.prev_top_facing = src.top().facing;

    if (m.kind == MoveKind::Rotate) {
        uint8_t d = src.size - 1;
        h ^= zobrist_cell[d][ssq][canonical_code64(src.stack[d])];
        src.stack[d].facing = static_cast<uint8_t>((src.stack[d].facing + m.rotate_after) & 3);
        h ^= zobrist_cell[d][ssq][canonical_code64(src.stack[d])];
        pos.side_to_move = other_side(side);
        pos.zobrist = h;
        return;
    }

    int dsq = m.dst;
    Cell& dst = pos.cell[dsq];

    if (m.kind == MoveKind::Single) {
        uint8_t src_top_idx = src.size - 1;
        Piece moving = src.stack[src_top_idx];
        h ^= zobrist_cell[src_top_idx][ssq][canonical_code64(moving)];
        src.size--;

        if (!dst.empty() && dst.owner() != side) {
            u.captured_size = dst.size;
            for (uint8_t i = 0; i < dst.size; ++i) {
                Piece cp = dst.stack[i];
                u.captured[i] = cp;
                h ^= zobrist_cell[i][dsq][canonical_code64(cp)];
                uint8_t old_cnt = pos.prisoners[side][cp.type];
                pos.prisoners[side][cp.type] = old_cnt + 1;
                h ^= zobrist_prisoner[side][cp.type][old_cnt] ^ zobrist_prisoner[side][cp.type][old_cnt + 1];
            }
            dst.size = 0;
        }

        if (m.rotate_after != 0) {
            moving.facing = static_cast<uint8_t>((moving.facing + m.rotate_after) & 3);
        }
        h ^= zobrist_cell[dst.size][dsq][canonical_code64(moving)];
        dst.stack[dst.size++] = moving;

        if (moving.type == VUONG) {
            pos.king_sq[side] = static_cast<uint8_t>(dsq);
        }
    } else { // MoveKind::Team
        Piece moving[MAX_STACK_SIZE];
        uint8_t moving_size = src.size;
        for (uint8_t i = 0; i < moving_size; ++i) {
            moving[i] = src.stack[i];
            h ^= zobrist_cell[i][ssq][canonical_code64(moving[i])];
        }
        src.size = 0;

        if (!dst.empty() && dst.owner() != side) {
            u.captured_size = dst.size;
            for (uint8_t i = 0; i < dst.size; ++i) {
                Piece cp = dst.stack[i];
                u.captured[i] = cp;
                h ^= zobrist_cell[i][dsq][canonical_code64(cp)];
                uint8_t old_cnt = pos.prisoners[side][cp.type];
                pos.prisoners[side][cp.type] = old_cnt + 1;
                h ^= zobrist_prisoner[side][cp.type][old_cnt] ^ zobrist_prisoner[side][cp.type][old_cnt + 1];
            }
            dst.size = 0;
        }

        if (m.rotate_after != 0) {
            moving[moving_size - 1].facing = static_cast<uint8_t>((moving[moving_size - 1].facing + m.rotate_after) & 3);
        }

        for (uint8_t i = 0; i < moving_size; ++i) {
            h ^= zobrist_cell[dst.size][dsq][canonical_code64(moving[i])];
            dst.stack[dst.size++] = moving[i];
        }
    }

    pos.side_to_move = other_side(side);
    pos.zobrist = h;
}

void undo_move(Position& pos, const Move& m, const UndoState& u) {
    Side side = other_side(static_cast<Side>(pos.side_to_move));
    pos.side_to_move = side;
    pos.zobrist = u.prev_zobrist;
    pos.king_sq[0] = u.prev_king_sq[0];
    pos.king_sq[1] = u.prev_king_sq[1];

    if (m.kind == MoveKind::Drop) {
        Cell& dst = pos.cell[m.dst];
        dst.size--;
        if (pos.reserve[side] != u.prev_reserve[side]) {
            pos.reserve[side] = u.prev_reserve[side];
        } else {
            pos.prisoners[side][m.ptype]++;
        }
        return;
    }

    Cell& src = pos.cell[m.src];
    if (m.kind == MoveKind::Rotate) {
        src.top().facing = u.prev_top_facing;
        return;
    }

    Cell& dst = pos.cell[m.dst];
    if (m.kind == MoveKind::Single) {
        Piece moved = dst.stack[--dst.size];
        moved.facing = u.prev_top_facing;
        src.stack[src.size++] = moved;
        if (u.captured_size > 0) {
            for (uint8_t i = 0; i < u.captured_size; ++i) {
                dst.stack[i] = u.captured[i];
                pos.prisoners[side][u.captured[i].type]--;
            }
            dst.size = u.captured_size;
        }
    } else { // MoveKind::Team
        // For team move, dst was either empty or enemy (captured), so dst currently holds ONLY the moved stack
        uint8_t moved_size = dst.size;
        for (uint8_t i = 0; i < moved_size; ++i) {
            src.stack[i] = dst.stack[i];
        }
        src.size = moved_size;
        src.top().facing = u.prev_top_facing;

        if (u.captured_size > 0) {
            for (uint8_t i = 0; i < u.captured_size; ++i) {
                dst.stack[i] = u.captured[i];
                pos.prisoners[side][u.captured[i].type]--;
            }
            dst.size = u.captured_size;
        } else {
            dst.size = 0;
        }
    }
}

void generate_legal_moves(Position& pos, MoveList& out) {
    generate_pseudo_moves(pos, out);
    Side mover = static_cast<Side>(pos.side_to_move);
    bool currently_in_check = is_in_check(pos, mover);
    int ksq = pos.king_sq[mover];
    int kr = (ksq < NUM_SQUARES) ? sq_row(ksq) : -1;
    int kc = (ksq < NUM_SQUARES) ? sq_col(ksq) : -1;

    UndoState u;
    int write_idx = 0;
    for (int i = 0; i < out.count; ++i) {
        const Move m = out.moves[i];
        if (!currently_in_check && kr >= 0) {
            if (m.kind == MoveKind::Drop || m.kind == MoveKind::Rotate) {
                // Dropping or rotating in place never empties a square or moves the king
                out.moves[write_idx++] = m;
                continue;
            }
            int ssq = m.src;
            if (ssq != ksq) {
                bool empties_src = (m.kind == MoveKind::Team) || (pos.cell[ssq].size == 1);
                if (!empties_src) {
                    out.moves[write_idx++] = m;
                    continue;
                }
                int sr = sq_row(ssq);
                int sc = sq_col(ssq);
                int dr = std::abs(sr - kr);
                int dc = std::abs(sc - kc);
                if (dr != 0 && dc != 0 && dr != dc) {
                    out.moves[write_idx++] = m;
                    continue;
                }
            }
        }
        apply_move(pos, m, u);
        if (!is_in_check(pos, mover)) {
            out.moves[write_idx++] = m;
        }
        undo_move(pos, m, u);
    }
    out.count = write_idx;
}

std::vector<Move> generate_legal_moves(const Position& pos) {
    Position copy_pos = pos;
    MoveList list;
    generate_legal_moves(copy_pos, list);
    return std::vector<Move>(list.begin(), list.end());
}

uint64_t perft(Position& pos, int depth) {
    if (depth <= 0) return 1ULL;
    MoveList legal;
    generate_legal_moves(pos, legal);
    if (depth == 1) return static_cast<uint64_t>(legal.count);
    uint64_t nodes = 0;
    UndoState u;
    for (int i = 0; i < legal.count; ++i) {
        apply_move(pos, legal.moves[i], u);
        nodes += perft(pos, depth - 1);
        undo_move(pos, legal.moves[i], u);
    }
    return nodes;
}

} // namespace homei

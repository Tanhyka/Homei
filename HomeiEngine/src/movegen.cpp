#include "movegen.h"
#include "attack.h"
#include "bitboard.h"
#include "eval.h"
#include "geometry.h"
#include "zobrist.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace homei {

namespace {

inline uint8_t canonical_code64(const Piece& p) {
    uint8_t f = can_rotate(p.type) ? p.facing : initial_facing(static_cast<Side>(p.owner));
    return static_cast<uint8_t>(((p.owner * 8 + p.type) * 4) + f);
}

inline bool drop_attacks_enemy_piece(const Position& pos, int dsq, uint8_t pt, uint8_t f, Side enemy) {
    return (piece_attacks_bb(pos, dsq, pt, f) & pos.bb_side[enemy]).any();
}

int drop_plain_facing_score(int dsq, uint8_t pt, uint8_t f, Side stm, Side enemy, int enemy_king) {
    int q = 0;
    uint8_t fwd = initial_facing(stm);
    uint8_t bwd = initial_facing(enemy);
    if (f == fwd) q += 40;
    else if (f == bwd) q -= 30;
    else q += 10;

    const DeadVecInfo& dvi = get_dead_vec_table()[pt][f][dsq];
    if (dvi.in_board == 0) q -= 200;
    q -= dvi.out_board * 25;

    if (enemy_king >= 0 && enemy_king < NUM_SQUARES) {
        int r = sq_row(dsq);
        int c = sq_col(dsq);
        int ekr = sq_row(enemy_king);
        int ekc = sq_col(enemy_king);
        if ((f == 0 && ekr < r) || (f == 2 && ekr > r) || (f == 1 && ekc > c) || (f == 3 && ekc < c)) {
            q += 60;
        }
    }
    return q;
}

// Exact O(1) bitboard check that a pinned piece at `src` moving to `dst` keeps own King safe.
// In CLP, KI is the ONLY piece that can jump distance-2 over its own King or over the pinner along BB_LINE[ksq][src]!
inline bool pinned_move_keeps_king_safe(
    const Position& pos, int ksq, int src, int dst, Side enemy
) {
    if (!BB_LINE[ksq][src].test_bit(dst)) return false;
    if (pos.top_piece[src].type != KI) return true;
    if (BB_BETWEEN[src][dst].test_bit(ksq)) return false; // jumped over own King!
    const Bitboard occ_after = (pos.bb_occ ^ Bitboard::from_sq(src)) | Bitboard::from_sq(dst);
    const bool is_ortho = (sq_row(ksq) == sq_row(src) || sq_col(ksq) == sq_col(src));
    Bitboard ray_att = (is_ortho ? ortho_attacks(ksq, occ_after) : diag_attacks(ksq, occ_after)) & BB_LINE[ksq][src];
    ray_att.clear_bit(dst); // dst is either empty-captured or our own KI sitting at dst
    if ((ray_att & pos.bb_side[enemy]).empty()) return true;
    return !is_attacked_with_occ(pos, ksq, enemy, occ_after);
}

// Bitboard-exact legality check for a single pseudo-legal move given precomputed `chk` and `pin_blockers`.
// Assumes pos.king_sq[mover] < NUM_SQUARES and pos.height[ksq] == 1.
inline bool is_legal_given_info(
    const Position& pos,
    const Move& m,
    Side mover,
    Side enemy,
    int ksq,
    const Bitboard& chk,
    int single_chk_sq,
    const Bitboard& interpose_bb,
    const Bitboard& pin_blockers
) {
    if (chk.empty()) {
        if (m.kind == MoveKind::Drop || m.kind == MoveKind::Rotate) {
            return true;
        }
        if (m.src == ksq) {
            return !is_attacked_with_occ(pos, m.dst, enemy, pos.bb_occ ^ Bitboard::from_sq(ksq));
        }
        if (!pin_blockers.test_bit(m.src)) {
            return true;
        }
        if (m.kind == MoveKind::Single && pos.height[m.src] >= 2) {
            return true;
        }
        return pinned_move_keeps_king_safe(pos, ksq, m.src, m.dst, enemy);
    }

    // In check!
    if (m.kind == MoveKind::Rotate) {
        return false;
    }
    if (m.src == ksq) {
        return !is_attacked_with_occ(pos, m.dst, enemy, pos.bb_occ ^ Bitboard::from_sq(ksq));
    }
    if (single_chk_sq < 0) {
        // Double check: only King moves can be legal
        return false;
    }
    if (m.kind == MoveKind::Drop) {
        return interpose_bb.test_bit(m.dst);
    }
    // Non-king Single or Team move when facing a single checker
    if (m.dst != single_chk_sq && !interpose_bb.test_bit(m.dst)) {
        return false;
    }
    if (pin_blockers.test_bit(m.src) && (m.kind == MoveKind::Team || pos.height[m.src] == 1)) {
        return false;
    }
    return true;
}

} // namespace

int get_raw_destinations(const Position& pos, int src_sq, int out_dsts[32]) {
    if (src_sq < 0 || src_sq >= NUM_SQUARES || pos.height[src_sq] == 0) return 0;

    const Piece top = pos.top_piece[src_sq];
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
                if (pos.height[dsq] > 0) break;
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
                uint8_t dh = pos.height[dsq];
                if (dh == 0 || (pos.cell_owner(dsq) == side && pos.top_piece[dsq].type != VUONG && dh < MAX_STACK_SIZE)) {
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
            uint8_t dh = pos.height[dsq];
            if (dh > 0) {
                if (pos.cell_owner(dsq) != side || pos.cell_contains_king(dsq) || dh >= MAX_STACK_SIZE) {
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

    // 3. Board moves: iterate squares owned by `side` via bitboard
    constexpr uint8_t rots_rotatable[3] = {0, 1, 3};
    constexpr uint8_t rots_fixed[1] = {0};
    Side enemy = other_side(side);
    Bitboard enemy_king_mask;
    if (pos.king_sq[enemy] < NUM_SQUARES) {
        enemy_king_mask.set_bit(pos.king_sq[enemy]);
    }
    Bitboard valid_mask = ~enemy_king_mask;

    Bitboard side_bb = pos.bb_side[side];
    while (side_bb.any()) {
        int ssq = side_bb.pop_lsb();
        const Piece top = pos.top_piece[ssq];
        uint8_t sh = pos.height[ssq];
        bool is_stack = (sh > 1);
        bool src_has_king = pos.cell_contains_king(ssq);
        bool top_rot = can_rotate(top.type);
        const uint8_t* rots = top_rot ? rots_rotatable : rots_fixed;
        int num_rots = top_rot ? 3 : 1;

        Bitboard targets = piece_attacks_bb(pos, ssq, top.type, top.facing) & valid_mask;
        while (targets.any()) {
            int dsq = targets.pop_lsb();
            uint8_t dh = pos.height[dsq];
            bool is_own = (dh > 0 && pos.cell_owner(dsq) == side);
            if (!is_own && dh > 0 && pos.cell_contains_king(dsq)) {
                continue;
            }

            bool can_single = !is_own;
            if (is_own) {
                can_single = (!is_stack && top.type != VUONG && !pos.cell_contains_king(dsq) && dh < MAX_STACK_SIZE);
            }
            bool can_team = (is_stack && !is_own && !src_has_king);

            if (can_single) {
                for (int ri = 0; ri < num_rots; ++ri) {
                    out.push(Move{MoveKind::Single, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rots[ri]});
                }
            }
            if (can_team) {
                for (int ri = 0; ri < num_rots; ++ri) {
                    out.push(Move{MoveKind::Team, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rots[ri]});
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
#ifdef HOMEI_PROFILE
    SearchProfile& prof = get_search_profile();
    const bool prof_on = prof.enabled;
    std::chrono::steady_clock::time_point t0;
    if (prof_on) {
        ++prof.movegen_cap_calls;
        t0 = std::chrono::steady_clock::now();
    }
#endif

    out.clear();
    Side side = static_cast<Side>(pos.side_to_move);
    if (pos.reserve[side] != 0) {
#ifdef HOMEI_PROFILE
        if (prof_on) {
            auto t1 = std::chrono::steady_clock::now();
            prof.movegen_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
            );
        }
#endif
        return;
    }

    Side enemy = other_side(side);
    Bitboard enemy_targets = pos.bb_side[enemy];
    if (pos.king_sq[enemy] < NUM_SQUARES) {
        enemy_targets.clear_bit(pos.king_sq[enemy]);
    }
    if (enemy_targets.empty()) {
#ifdef HOMEI_PROFILE
        if (prof_on) {
            auto t1 = std::chrono::steady_clock::now();
            prof.movegen_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
            );
        }
#endif
        return;
    }

    constexpr uint8_t rots_rotatable[3] = {0, 1, 3};
    constexpr uint8_t rots_fixed[1] = {0};

    Bitboard side_bb = pos.bb_side[side];
    while (side_bb.any()) {
        int ssq = side_bb.pop_lsb();
        const Piece top = pos.top_piece[ssq];
        Bitboard caps = piece_attacks_bb(pos, ssq, top.type, top.facing) & enemy_targets;
        if (caps.empty()) continue;

        bool is_stack = (pos.height[ssq] > 1);
        bool src_has_king = pos.cell_contains_king(ssq);
        bool top_rot = can_rotate(top.type);
        const uint8_t* rots = top_rot ? rots_rotatable : rots_fixed;
        int num_rots = top_rot ? 3 : 1;
        bool can_team = (is_stack && !src_has_king);

        while (caps.any()) {
            int dsq = caps.pop_lsb();
            if (pos.cell_contains_king(dsq)) continue;

            // Emit all Single rots consecutively, then all Team rots consecutively so
            // callers can cache (kind, src, dst) legality & SEE across all 3 rotations!
            for (int ri = 0; ri < num_rots; ++ri) {
                out.push(Move{MoveKind::Single, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rots[ri]});
            }
            if (can_team) {
                for (int ri = 0; ri < num_rots; ++ri) {
                    out.push(Move{MoveKind::Team, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rots[ri]});
                }
            }
        }
    }

#ifdef HOMEI_PROFILE
    if (prof_on) {
        auto t1 = std::chrono::steady_clock::now();
        prof.movegen_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
        );
    }
#endif
}

void generate_quiet_and_drop_moves(const Position& pos, MoveList& out) {
    generate_quiet_and_drop_moves(pos, out, QuietGenFilter{});
}

void generate_quiet_and_drop_moves(const Position& pos, MoveList& out, const QuietGenFilter& filter) {
#ifdef HOMEI_PROFILE
    SearchProfile& prof = get_search_profile();
    const bool prof_on = prof.enabled;
    std::chrono::steady_clock::time_point t0;
    if (prof_on) {
        t0 = std::chrono::steady_clock::now();
    }
#endif

    out.clear();
    Side side = static_cast<Side>(pos.side_to_move);
    Side enemy = other_side(side);
    uint8_t base_f = initial_facing(side);

    // 1. Reserve phase: only reserve drops
    if (pos.reserve[side] != 0) {
        int drop_row = (side == CAM) ? 1 : 7;
        for (uint8_t pt : {LONG, PHUONG}) {
            if (!pos.has_reserve_type(side, pt)) continue;
            for (int c = 0; c < BOARD_SIZE; ++c) {
                int dsq = make_sq(drop_row, c);
                uint8_t dh = pos.height[dsq];
                if (dh == 0 || (pos.cell_owner(dsq) == side && pos.top_piece[dsq].type != VUONG && dh < MAX_STACK_SIZE)) {
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
#ifdef HOMEI_PROFILE
        if (prof_on) {
            auto t1 = std::chrono::steady_clock::now();
            prof.movegen_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
            );
        }
#endif
        return;
    }

    // 2. Prisoner drops
    uint8_t has_any_prisoner = 0;
    for (uint8_t pt = 0; pt < 8; ++pt) {
        has_any_prisoner |= pos.prisoners[side][pt];
    }
    if (has_any_prisoner != 0) {
        int drop_sqs[NUM_SQUARES];
        int ndrop = 0;
        Bitboard cand_bb = ~pos.bb_side[enemy];
        while (cand_bb.any()) {
            int dsq = cand_bb.pop_lsb();
            uint8_t dh = pos.height[dsq];
            if (dh == 0 || (!pos.cell_contains_king(dsq) && dh < MAX_STACK_SIZE)) {
                drop_sqs[ndrop++] = dsq;
            }
        }

        const bool use_filter = filter.skip_far_noncheck_drops || (filter.max_plain_drop_facings < 4);
        if (!use_filter) {
            for (uint8_t pt = 0; pt < 8; ++pt) {
                if (pos.prisoners[side][pt] == 0) continue;
                bool rot = can_rotate(pt);
                for (int di = 0; di < ndrop; ++di) {
                    int dsq = drop_sqs[di];
                    if (rot) {
                        for (int8_t f = 0; f < 4; ++f) {
                            out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, f, 0});
                        }
                    } else {
                        out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, static_cast<int8_t>(base_f), 0});
                    }
                }
            }
        } else {
            int own_king = pos.king_sq[side];
            int enemy_king = pos.king_sq[enemy];
            int okr = (own_king < NUM_SQUARES) ? sq_row(own_king) : -1;
            int okc = (own_king < NUM_SQUARES) ? sq_col(own_king) : -1;
            int ekr = (enemy_king < NUM_SQUARES) ? sq_row(enemy_king) : -1;
            int ekc = (enemy_king < NUM_SQUARES) ? sq_col(enemy_king) : -1;
            int max_plain = std::clamp(filter.max_plain_drop_facings, 1, 4);

            for (int di = 0; di < ndrop; ++di) {
                int dsq = drop_sqs[di];
                int r = sq_row(dsq);
                int c = sq_col(dsq);
                bool is_far = false;
                if (filter.skip_far_noncheck_drops) {
                    int d_own = (okr >= 0) ? std::max(std::abs(r - okr), std::abs(c - okc)) : 99;
                    int d_enemy = (ekr >= 0) ? std::max(std::abs(r - ekr), std::abs(c - ekc)) : 99;
                    is_far = (d_own > 2 && d_enemy > 3);
                }

                for (uint8_t pt = 0; pt < 8; ++pt) {
                    if (pos.prisoners[side][pt] == 0) continue;
                    bool rot = can_rotate(pt);
                    if (is_far && pt != XA && pt != LONG && pt != PHUONG) {
                        continue;
                    }
                    if (!rot) {
                        if (is_far) {
                            if (enemy_king >= NUM_SQUARES || !piece_attacks_sq(pos, dsq, pt, base_f, enemy_king)) {
                                continue;
                            }
                        }
                        out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, static_cast<int8_t>(base_f), 0});
                    } else if (is_far) {
                        for (int8_t f = 0; f < 4; ++f) {
                            if (enemy_king < NUM_SQUARES && piece_attacks_sq(pos, dsq, pt, static_cast<uint8_t>(f), enemy_king)) {
                                out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, f, 0});
                            }
                        }
                    } else if (max_plain < 4) {
                        int8_t plain_f[4]{};
                        int plain_q[4]{};
                        int n_plain = 0;
                        const Bitboard xa_ortho = (pt == XA) ? ortho_attacks(dsq, pos.bb_occ) : Bitboard{};
                        const Bitboard enemy_bb = pos.bb_side[enemy];
                        for (int8_t f = 0; f < 4; ++f) {
                            uint8_t uf = static_cast<uint8_t>(f);
                            Bitboard att = (pt == XA) ? (xa_ortho & BB_RAY_ORTHO[dsq][uf]) : BB_STEP_ATTACKS[pt][uf][dsq];
                            if ((att & enemy_bb).any()) {
                                out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, f, 0});
                            } else {
                                plain_f[n_plain] = f;
                                plain_q[n_plain] = drop_plain_facing_score(dsq, pt, uf, side, enemy, enemy_king);
                                ++n_plain;
                            }
                        }
                        int emit_n = std::min(n_plain, max_plain);
                        for (int k = 0; k < emit_n; ++k) {
                            int best = k;
                            for (int j = k + 1; j < n_plain; ++j) {
                                if (plain_q[j] > plain_q[best]) best = j;
                            }
                            if (best != k) {
                                std::swap(plain_q[k], plain_q[best]);
                                std::swap(plain_f[k], plain_f[best]);
                            }
                            out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, plain_f[k], 0});
                        }
                    } else {
                        for (int8_t f = 0; f < 4; ++f) {
                            out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, f, 0});
                        }
                    }
                }
            }
        }
    }

    // 3. Quiet board moves (non-captures) and rotations
    constexpr uint8_t rots_rotatable[3] = {0, 1, 3};
    constexpr uint8_t rots_fixed[1] = {0};
    Bitboard non_enemy_mask = ~pos.bb_side[enemy];

    Bitboard side_bb = pos.bb_side[side];
    while (side_bb.any()) {
        int ssq = side_bb.pop_lsb();
        const Piece top = pos.top_piece[ssq];
        uint8_t sh = pos.height[ssq];
        bool is_stack = (sh > 1);
        bool src_has_king = pos.cell_contains_king(ssq);
        bool top_rot = can_rotate(top.type);
        const uint8_t* rots = top_rot ? rots_rotatable : rots_fixed;
        int num_rots = top_rot ? 3 : 1;

        Bitboard quiets = piece_attacks_bb(pos, ssq, top.type, top.facing) & non_enemy_mask;
        while (quiets.any()) {
            int dsq = quiets.pop_lsb();
            uint8_t dh = pos.height[dsq];
            bool is_own = (dh > 0);

            bool can_single = true;
            if (is_own) {
                can_single = (!is_stack && top.type != VUONG && !pos.cell_contains_king(dsq) && dh < MAX_STACK_SIZE);
            }
            bool can_team = (is_stack && !is_own && !src_has_king);

            if (can_single) {
                for (int ri = 0; ri < num_rots; ++ri) {
                    out.push(Move{MoveKind::Single, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rots[ri]});
                }
            }
            if (can_team) {
                for (int ri = 0; ri < num_rots; ++ri) {
                    out.push(Move{MoveKind::Team, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rots[ri]});
                }
            }
        }

        if (top_rot) {
            out.push(Move{MoveKind::Rotate, static_cast<int8_t>(ssq), -1, top.type, -1, 1});
            out.push(Move{MoveKind::Rotate, static_cast<int8_t>(ssq), -1, top.type, -1, 3});
        }
    }

#ifdef HOMEI_PROFILE
    if (prof_on) {
        auto t1 = std::chrono::steady_clock::now();
        prof.movegen_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
        );
    }
#endif
}

bool is_pseudo_legal_move(const Position& pos, const Move& m) {
    if (m.is_none() || m.ptype >= 8) return false;
    Side side = static_cast<Side>(pos.side_to_move);
    uint8_t base_f = initial_facing(side);

    if (pos.reserve[side] != 0) {
        if (m.kind != MoveKind::Drop || m.src != -1 || m.rotate_after != 0) return false;
        if (m.ptype != LONG && m.ptype != PHUONG) return false;
        if (!pos.has_reserve_type(side, m.ptype)) return false;
        if (m.dst < 0 || m.dst >= NUM_SQUARES) return false;
        int drop_row = (side == CAM) ? 1 : 7;
        if (sq_row(m.dst) != drop_row) return false;
        uint8_t dh = pos.height[m.dst];
        if (dh > 0) {
            if (pos.cell_owner(m.dst) != side || pos.top_piece[m.dst].type == VUONG || dh >= MAX_STACK_SIZE) {
                return false;
            }
        }
        return m.facing == static_cast<int8_t>(base_f);
    }

    if (m.kind == MoveKind::Drop) {
        if (m.src != -1 || m.rotate_after != 0) return false;
        if (m.dst < 0 || m.dst >= NUM_SQUARES) return false;
        if (pos.prisoners[side][m.ptype] == 0) return false;
        uint8_t dh = pos.height[m.dst];
        if (dh > 0) {
            if (pos.cell_owner(m.dst) != side || pos.cell_contains_king(m.dst) || dh >= MAX_STACK_SIZE) {
                return false;
            }
        }
        if (can_rotate(m.ptype)) {
            return m.facing >= 0 && m.facing < 4;
        }
        return m.facing == static_cast<int8_t>(base_f);
    }

    if (m.kind == MoveKind::Rotate) {
        if (m.src < 0 || m.src >= NUM_SQUARES || m.dst != -1 || m.facing != -1) return false;
        if (m.rotate_after != 1 && m.rotate_after != 3) return false;
        if (pos.height[m.src] == 0 || pos.cell_owner(m.src) != side) return false;
        if (pos.top_piece[m.src].type != m.ptype || !can_rotate(m.ptype)) return false;
        return true;
    }

    if (m.kind == MoveKind::Single || m.kind == MoveKind::Team) {
        if (m.src < 0 || m.src >= NUM_SQUARES || m.dst < 0 || m.dst >= NUM_SQUARES || m.facing != -1) return false;
        if (m.src == m.dst) return false;
        uint8_t sh = pos.height[m.src];
        if (sh == 0 || pos.cell_owner(m.src) != side || pos.top_piece[m.src].type != m.ptype) return false;
        if (can_rotate(m.ptype)) {
            if (m.rotate_after != 0 && m.rotate_after != 1 && m.rotate_after != 3) return false;
        } else {
            if (m.rotate_after != 0) return false;
        }

        uint8_t dh = pos.height[m.dst];
        bool is_own = (dh > 0 && pos.cell_owner(m.dst) == side);
        bool is_capture = (dh > 0 && !is_own);
        if (is_capture && pos.cell_contains_king(m.dst)) return false;

        if (m.kind == MoveKind::Single) {
            if (is_own) {
                if (sh > 1 || m.ptype == VUONG || pos.cell_contains_king(m.dst) || dh >= MAX_STACK_SIZE) {
                    return false;
                }
            }
        } else {
            if (sh <= 1 || is_own || pos.cell_contains_king(m.src)) {
                return false;
            }
        }

        return piece_attacks_sq(pos, m.src, m.ptype, pos.top_piece[m.src].facing, m.dst);
    }

    return false;
}

bool check_move_leaves_king_safe_pinned(const Position& pos, const Move& m, const Bitboard& pin_blockers) {
    Side mover = static_cast<Side>(pos.side_to_move);
    int ksq = pos.king_sq[mover];
    if (ksq >= NUM_SQUARES) return true;
    if (m.kind == MoveKind::Drop || m.kind == MoveKind::Rotate) return true;
    if (m.src == ksq) {
        if (pos.height[ksq] > 1) {
            Position copy = pos;
            UndoState u;
            apply_move(copy, m, u);
            return !is_in_check(copy, mover);
        }
        return !is_attacked_with_occ(pos, m.dst, other_side(mover), pos.bb_occ ^ Bitboard::from_sq(ksq));
    }
    if (!pin_blockers.test_bit(m.src)) return true;
    if (m.kind == MoveKind::Single && pos.height[m.src] >= 2) return true;
    return pinned_move_keeps_king_safe(pos, ksq, m.src, m.dst, other_side(mover));
}

bool check_move_leaves_king_safe(const Position& pos, const Move& m, bool currently_in_check) {
    Side mover = static_cast<Side>(pos.side_to_move);
    Side enemy = other_side(mover);
    int ksq = pos.king_sq[mover];
    if (ksq >= NUM_SQUARES) return true;

    if (pos.height[ksq] > 1) {
        Position copy = pos;
        UndoState u;
        apply_move(copy, m, u);
        return !is_in_check(copy, mover);
    }

    if (!currently_in_check) {
        if (m.kind == MoveKind::Drop || m.kind == MoveKind::Rotate) return true;
        if (m.src == ksq) {
            return !is_attacked_with_occ(pos, m.dst, enemy, pos.bb_occ ^ Bitboard::from_sq(ksq));
        }
        if (m.kind == MoveKind::Single && pos.height[m.src] >= 2) return true;
        if (BB_LINE[ksq][m.src].empty()) return true;
        if (pos.top_piece[m.src].type != KI && BB_LINE[ksq][m.src].test_bit(m.dst)) return true;
        Bitboard pinners;
        Bitboard blockers = slider_blockers(pos, mover, pinners);
        if (!blockers.test_bit(m.src)) return true;
        return pinned_move_keeps_king_safe(pos, ksq, m.src, m.dst, enemy);
    }

    // In check!
    if (m.kind == MoveKind::Rotate) return false;
    if (m.src == ksq) {
        return !is_attacked_with_occ(pos, m.dst, enemy, pos.bb_occ ^ Bitboard::from_sq(ksq));
    }
    Bitboard chk = checkers_bb(pos, mover);
    if (chk.empty()) return true;
    if (chk.more_than_one()) return false;
    int chk_sq = chk.lsb();
    uint8_t chk_pt = pos.top_piece[chk_sq].type;
    Bitboard interpose = (chk_pt == LONG || chk_pt == PHUONG || chk_pt == XA) ? BB_BETWEEN[ksq][chk_sq] : Bitboard{};
    if (m.kind == MoveKind::Drop) {
        return interpose.test_bit(m.dst);
    }
    if (m.dst != chk_sq && !interpose.test_bit(m.dst)) return false;
    if (m.kind == MoveKind::Single && pos.height[m.src] >= 2) return true;
    if (BB_LINE[ksq][m.src].empty()) return true;
    Bitboard pinners;
    Bitboard blockers = slider_blockers(pos, mover, pinners);
    return !blockers.test_bit(m.src);
}

bool is_legal_move_fast(const Position& pos, const Move& m, bool currently_in_check) {
    if (!is_pseudo_legal_move(pos, m)) return false;
    return check_move_leaves_king_safe(pos, m, currently_in_check);
}

void apply_move(Position& pos, const Move& m, UndoState& u) {
    Side side = static_cast<Side>(pos.side_to_move);
    u.prev_reserve[0] = pos.reserve[0];
    u.prev_reserve[1] = pos.reserve[1];
    u.prev_king_sq[0] = pos.king_sq[0];
    u.prev_king_sq[1] = pos.king_sq[1];
    u.prev_board_key = pos.board_key;
    u.prev_hand_key = pos.hand_key;
    u.prev_zobrist = pos.zobrist;
    u.captured_size = 0;

    uint64_t bk = pos.board_key ^ zobrist_side;
    uint64_t hk = pos.hand_key;

    if (m.kind == MoveKind::Drop) {
        int dsq = m.dst;
        uint8_t pt = m.ptype;
        uint8_t f = static_cast<uint8_t>((m.facing >= 0 ? m.facing : 0) & 3);
        Piece dropped(pt, side, f);

        if (pos.has_reserve_type(side, pt)) {
            uint8_t old_mask = pos.reserve[side] & 3;
            pos.set_reserve_type(side, pt, false);
            uint8_t new_mask = pos.reserve[side] & 3;
            hk = hk - zobrist_reserve[side][old_mask] + zobrist_reserve[side][new_mask];
        } else {
            pos.prisoners[side][pt]--;
            hk -= zobrist_prisoner_unit[side][pt];
        }

        uint8_t dh = pos.height[dsq];
        if (dh > 0) {
            pos.bb_remove_top(pos.cell_owner(dsq), pos.top_piece[dsq].type, dsq);
        }
        bk ^= zobrist_cell_key(dh, dsq, canonical_code64(dropped));
        pos.cell_push(dsq, dropped);
        pos.bb_add_top(side, pt, dsq);

        pos.side_to_move = other_side(side);
        pos.board_key = bk;
        pos.hand_key = hk;
        pos.zobrist = bk ^ hk;
        return;
    }

    int ssq = m.src;
    u.prev_top_facing = pos.top_piece[ssq].facing;

    if (m.kind == MoveKind::Rotate) {
        uint8_t d = static_cast<uint8_t>(pos.height[ssq] - 1);
        bk ^= zobrist_cell_key(d, ssq, canonical_code64(pos.top_piece[ssq]));
        pos.top_piece[ssq].facing = static_cast<uint8_t>((pos.top_piece[ssq].facing + m.rotate_after) & 3);
        bk ^= zobrist_cell_key(d, ssq, canonical_code64(pos.top_piece[ssq]));

        pos.side_to_move = other_side(side);
        pos.board_key = bk;
        pos.hand_key = hk;
        pos.zobrist = bk ^ hk;
        return;
    }

    int dsq = m.dst;

    if (m.kind == MoveKind::Single) {
        uint8_t src_top_idx = static_cast<uint8_t>(pos.height[ssq] - 1);
        Piece moving = pos.top_piece[ssq];
        bk ^= zobrist_cell_key(src_top_idx, ssq, canonical_code64(moving));
        pos.bb_remove_top(side, moving.type, ssq);
        pos.cell_pop(ssq);
        if (pos.height[ssq] > 0) {
            pos.bb_add_top(pos.cell_owner(ssq), pos.top_piece[ssq].type, ssq);
        }

        uint8_t dh = pos.height[dsq];
        if (dh > 0) {
            Side dst_owner = pos.cell_owner(dsq);
            pos.bb_remove_top(dst_owner, pos.top_piece[dsq].type, dsq);
            if (dst_owner != side) {
                u.captured_size = dh;
                // Record bottom-to-top in u.captured[0..dh-1] and clear cell
                Piece top_cap = pos.top_piece[dsq];
                u.captured[dh - 1] = top_cap;
                bk ^= zobrist_cell_key(static_cast<uint8_t>(dh - 1), dsq, canonical_code64(top_cap));
                pos.prisoners[side][top_cap.type]++;
                hk += zobrist_prisoner_unit[side][top_cap.type];
                if (dh >= 2) {
                    int ci = static_cast<int>(dh) - 2;
                    uint8_t idx = pos.buried_head[dsq];
                    while (idx != BURIED_NONE) {
                        Piece cp = pos.buried_piece[idx];
                        uint8_t nxt = pos.buried_next[idx];
                        pos.free_buried(idx);
                        u.captured[ci] = cp;
                        bk ^= zobrist_cell_key(static_cast<uint8_t>(ci), dsq, canonical_code64(cp));
                        pos.prisoners[side][cp.type]++;
                        hk += zobrist_prisoner_unit[side][cp.type];
                        --ci;
                        idx = nxt;
                    }
                    pos.bb_stack.clear_bit(dsq);
                }
                pos.height[dsq] = 0;
                pos.buried_head[dsq] = BURIED_NONE;
                dh = 0;
            }
        }

        if (m.rotate_after != 0) {
            moving.facing = static_cast<uint8_t>((moving.facing + m.rotate_after) & 3);
        }
        bk ^= zobrist_cell_key(dh, dsq, canonical_code64(moving));
        pos.cell_push(dsq, moving);
        pos.bb_add_top(side, moving.type, dsq);

        if (moving.type == VUONG) {
            pos.king_sq[side] = static_cast<uint8_t>(dsq);
        }
    } else { // MoveKind::Team — O(1) buried-head transfer!
        uint8_t moving_size = pos.height[ssq];
        Piece old_top = pos.top_piece[ssq];
        uint8_t moving_buried = pos.buried_head[ssq];
        pos.bb_remove_top(side, old_top.type, ssq);

        // Capture enemy stack on dst if present
        uint8_t dh = pos.height[dsq];
        if (dh > 0 && pos.cell_owner(dsq) != side) {
            pos.bb_remove_top(pos.cell_owner(dsq), pos.top_piece[dsq].type, dsq);
            u.captured_size = dh;
            Piece top_cap = pos.top_piece[dsq];
            u.captured[dh - 1] = top_cap;
            bk ^= zobrist_cell_key(static_cast<uint8_t>(dh - 1), dsq, canonical_code64(top_cap));
            pos.prisoners[side][top_cap.type]++;
            hk += zobrist_prisoner_unit[side][top_cap.type];
            if (dh >= 2) {
                int ci = static_cast<int>(dh) - 2;
                uint8_t idx = pos.buried_head[dsq];
                while (idx != BURIED_NONE) {
                    Piece cp = pos.buried_piece[idx];
                    uint8_t nxt = pos.buried_next[idx];
                    pos.free_buried(idx);
                    u.captured[ci] = cp;
                    bk ^= zobrist_cell_key(static_cast<uint8_t>(ci), dsq, canonical_code64(cp));
                    pos.prisoners[side][cp.type]++;
                    hk += zobrist_prisoner_unit[side][cp.type];
                    --ci;
                    idx = nxt;
                }
                pos.bb_stack.clear_bit(dsq);
            }
            pos.height[dsq] = 0;
            pos.buried_head[dsq] = BURIED_NONE;
        }

        Piece new_top = old_top;
        if (m.rotate_after != 0) {
            new_top.facing = static_cast<uint8_t>((new_top.facing + m.rotate_after) & 3);
        }

        // Update Zobrist for moved stack (top + buried chain)
        bk ^= zobrist_cell_key(static_cast<uint8_t>(moving_size - 1), ssq, canonical_code64(old_top));
        bk ^= zobrist_cell_key(static_cast<uint8_t>(moving_size - 1), dsq, canonical_code64(new_top));
        int d = static_cast<int>(moving_size) - 2;
        for (uint8_t idx = moving_buried; idx != BURIED_NONE && d >= 0; idx = pos.buried_next[idx], --d) {
            uint8_t c64 = canonical_code64(pos.buried_piece[idx]);
            bk ^= zobrist_cell_key(static_cast<uint8_t>(d), ssq, c64);
            bk ^= zobrist_cell_key(static_cast<uint8_t>(d), dsq, c64);
        }

        // O(1) transfer of the entire stack from ssq to dsq
        pos.height[ssq] = 0;
        pos.buried_head[ssq] = BURIED_NONE;
        pos.bb_stack.clear_bit(ssq);

        pos.top_piece[dsq] = new_top;
        pos.height[dsq] = moving_size;
        pos.buried_head[dsq] = moving_buried;
        if (moving_size >= 2) pos.bb_stack.set_bit(dsq);
        pos.bb_add_top(side, new_top.type, dsq);
    }

    pos.side_to_move = other_side(side);
    pos.board_key = bk;
    pos.hand_key = hk;
    pos.zobrist = bk ^ hk;
}

void undo_move(Position& pos, const Move& m, const UndoState& u) {
    Side side = other_side(static_cast<Side>(pos.side_to_move));
    pos.side_to_move = side;
    pos.board_key = u.prev_board_key;
    pos.hand_key = u.prev_hand_key;
    pos.zobrist = u.prev_zobrist;
    pos.king_sq[0] = u.prev_king_sq[0];
    pos.king_sq[1] = u.prev_king_sq[1];

    if (m.kind == MoveKind::Drop) {
        int dsq = m.dst;
        pos.bb_remove_top(side, m.ptype, dsq);
        pos.cell_pop(dsq);
        if (pos.height[dsq] > 0) {
            pos.bb_add_top(pos.cell_owner(dsq), pos.top_piece[dsq].type, dsq);
        }
        if (pos.reserve[side] != u.prev_reserve[side]) {
            pos.reserve[side] = u.prev_reserve[side];
        } else {
            pos.prisoners[side][m.ptype]++;
        }
        return;
    }

    int ssq = m.src;
    if (m.kind == MoveKind::Rotate) {
        pos.top_piece[ssq].facing = u.prev_top_facing;
        return;
    }

    int dsq = m.dst;
    if (m.kind == MoveKind::Single) {
        Piece moved = pos.cell_pop(dsq);
        pos.bb_remove_top(side, moved.type, dsq);
        moved.facing = u.prev_top_facing;
        if (pos.height[ssq] > 0) {
            pos.bb_remove_top(pos.cell_owner(ssq), pos.top_piece[ssq].type, ssq);
        }
        pos.cell_push(ssq, moved);
        pos.bb_add_top(side, moved.type, ssq);
        if (u.captured_size > 0) {
            for (uint8_t i = 0; i < u.captured_size; ++i) {
                pos.cell_push(dsq, u.captured[i]);
                pos.prisoners[side][u.captured[i].type]--;
            }
            pos.bb_add_top(pos.cell_owner(dsq), pos.top_piece[dsq].type, dsq);
        } else if (pos.height[dsq] > 0) {
            pos.bb_add_top(pos.cell_owner(dsq), pos.top_piece[dsq].type, dsq);
        }
    } else { // MoveKind::Team — O(1) reverse stack transfer!
        uint8_t moved_size = pos.height[dsq];
        Piece moved_top = pos.top_piece[dsq];
        uint8_t moved_buried = pos.buried_head[dsq];
        pos.bb_remove_top(side, moved_top.type, dsq);

        moved_top.facing = u.prev_top_facing;
        pos.top_piece[ssq] = moved_top;
        pos.height[ssq] = moved_size;
        pos.buried_head[ssq] = moved_buried;
        if (moved_size >= 2) pos.bb_stack.set_bit(ssq);
        pos.bb_add_top(side, moved_top.type, ssq);

        pos.height[dsq] = 0;
        pos.buried_head[dsq] = BURIED_NONE;
        pos.bb_stack.clear_bit(dsq);

        if (u.captured_size > 0) {
            for (uint8_t i = 0; i < u.captured_size; ++i) {
                pos.cell_push(dsq, u.captured[i]);
                pos.prisoners[side][u.captured[i].type]--;
            }
            pos.bb_add_top(pos.cell_owner(dsq), pos.top_piece[dsq].type, dsq);
        }
    }
}

void generate_legal_moves(const Position& pos, MoveList& out) {
#ifdef HOMEI_PROFILE
    SearchProfile& prof = get_search_profile();
    const bool prof_on = prof.enabled;
    std::chrono::steady_clock::time_point t0;
    if (prof_on) {
        ++prof.movegen_legal_calls;
        t0 = std::chrono::steady_clock::now();
    }
#endif

    generate_pseudo_moves(pos, out);
    Side mover = static_cast<Side>(pos.side_to_move);
    Side enemy = other_side(mover);
    int ksq = pos.king_sq[mover];

    if (ksq < NUM_SQUARES) {
        if (pos.height[ksq] == 1) {
            Bitboard chk = checkers_bb(pos, mover);
            int single_chk_sq = (chk.any() && !chk.more_than_one()) ? chk.lsb() : -1;
            Bitboard interpose;
            if (single_chk_sq >= 0) {
                uint8_t cpt = pos.top_piece[single_chk_sq].type;
                if (cpt == LONG || cpt == PHUONG || cpt == XA) {
                    interpose = BB_BETWEEN[ksq][single_chk_sq];
                }
            }
            Bitboard pinners;
            Bitboard pin_blockers = slider_blockers(pos, mover, pinners);

            int write_idx = 0;
            // Cache legality across consecutive rotate_after (0, 1, 3) for the same (kind, src, dst)
            MoveKind last_kind = MoveKind::Rotate;
            int8_t last_src = -2;
            int8_t last_dst = -2;
            bool last_ok = false;

            for (int i = 0; i < out.count; ++i) {
                const Move m = out.moves[i];
                bool ok;
                if (m.kind == last_kind && m.src == last_src && m.dst == last_dst && m.kind != MoveKind::Drop) {
                    ok = last_ok;
                } else {
                    ok = is_legal_given_info(pos, m, mover, enemy, ksq, chk, single_chk_sq, interpose, pin_blockers);
                    last_kind = m.kind;
                    last_src = m.src;
                    last_dst = m.dst;
                    last_ok = ok;
                }
                if (ok) {
                    out.moves[write_idx++] = m;
                }
            }
            out.count = write_idx;
        } else {
            // Synthetic edge case: buried King fallback
            Position copy = pos;
            UndoState u;
            int write_idx = 0;
            for (int i = 0; i < out.count; ++i) {
                const Move m = out.moves[i];
                apply_move(copy, m, u);
                if (!is_in_check(copy, mover)) {
                    out.moves[write_idx++] = m;
                }
                undo_move(copy, m, u);
            }
            out.count = write_idx;
        }
    }

#ifdef HOMEI_PROFILE
    if (prof_on) {
        auto t1 = std::chrono::steady_clock::now();
        prof.movegen_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
        );
    }
#endif
}

std::vector<Move> generate_legal_moves(const Position& pos) {
    MoveList list;
    generate_legal_moves(pos, list);
    return std::vector<Move>(list.begin(), list.end());
}

uint64_t perft(Position& pos, int depth) {
    if (depth <= 0) return 1ULL;
    MoveList list;
    generate_legal_moves(pos, list);
    if (depth == 1) return static_cast<uint64_t>(list.count);
    uint64_t nodes = 0;
    UndoState u;
    for (int i = 0; i < list.count; ++i) {
        apply_move(pos, list.moves[i], u);
        nodes += perft(pos, depth - 1);
        undo_move(pos, list.moves[i], u);
    }
    return nodes;
}

} // namespace homei

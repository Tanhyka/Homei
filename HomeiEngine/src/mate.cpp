#include "mate.h"
#include "attack.h"
#include "geometry.h"
#include "see.h"
#include "zobrist.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace homei {

namespace {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

constexpr uint32_t PN_INF = 100000000U;

struct RevStepEntry {
    uint8_t src_sq = 0;
    uint8_t ptype_mask_by_facing[4]{};
};

struct RevStepList {
    uint8_t count = 0;
    RevStepEntry entries[24]{};
};

RevStepList g_rev_step[NUM_SQUARES];
std::atomic<bool> g_mate_init{false};
std::once_flag g_mate_once;

void ensure_mate_init() {
    if (g_mate_init.load(std::memory_order_acquire)) return;
    std::call_once(g_mate_once, []() {
        init_geometry();
        init_attack_tables();

        for (int tsq = 0; tsq < NUM_SQUARES; ++tsq) {
            g_rev_step[tsq].count = 0;
        }

        for (int ssq = 0; ssq < NUM_SQUARES; ++ssq) {
            int sr = sq_row(ssq);
            int sc = sq_col(ssq);
            uint8_t masks_for_target[NUM_SQUARES][4]{};
            bool reaches[NUM_SQUARES]{};

            for (int pt = 0; pt < 8; ++pt) {
                for (int f = 0; f < 4; ++f) {
                    const auto& mt = get_piece_moves(static_cast<uint8_t>(pt), static_cast<uint8_t>(f));
                    for (uint8_t i = 0; i < mt.count; ++i) {
                        if (mt.vecs[i].sliding) continue;
                        int tr = sr + mt.vecs[i].dr;
                        int tc = sc + mt.vecs[i].dc;
                        if (in_bounds(tr, tc)) {
                            int tsq = make_sq(tr, tc);
                            masks_for_target[tsq][f] |= static_cast<uint8_t>(1U << pt);
                            reaches[tsq] = true;
                        }
                    }
                }
            }

            for (int tsq = 0; tsq < NUM_SQUARES; ++tsq) {
                if (reaches[tsq]) {
                    auto& list = g_rev_step[tsq];
                    auto& e = list.entries[list.count++];
                    e.src_sq = static_cast<uint8_t>(ssq);
                    for (int f = 0; f < 4; ++f) {
                        e.ptype_mask_by_facing[f] = masks_for_target[tsq][f];
                    }
                }
            }
        }
        g_mate_init.store(true, std::memory_order_release);
    });
}

inline bool piece_attacks_sq_ex(
    const Position& pos, int from_sq, uint8_t ptype, uint8_t facing, int target_sq, int ignore_sq = -1
) {
    if (from_sq < 0 || from_sq >= NUM_SQUARES || from_sq == target_sq) return false;
    const PieceMoveTable& tbl = get_piece_moves(ptype, facing);
    int r0 = sq_row(from_sq);
    int c0 = sq_col(from_sq);
    int rt = sq_row(target_sq);
    int ct = sq_col(target_sq);

    for (uint8_t i = 0; i < tbl.count; ++i) {
        const MoveVec& mv = tbl.vecs[i];
        if (!mv.sliding) {
            if (r0 + mv.dr == rt && c0 + mv.dc == ct) return true;
        } else {
            int nr = r0 + mv.dr;
            int nc = c0 + mv.dc;
            while (in_bounds(nr, nc)) {
                int s = make_sq(nr, nc);
                if (s == target_sq) return true;
                if (s != ignore_sq && pos.height[s] > 0) break;
                nr += mv.dr;
                nc += mv.dc;
            }
        }
    }
    return false;
}

inline bool local_is_attacked(const Position& pos, int target_sq, Side by_side) {
    return is_attacked(pos, target_sq, by_side);
}

inline int find_king_sq(const Position& pos, Side s) {
    uint8_t ksq = pos.king_sq[s];
    if (ksq < NUM_SQUARES) return ksq;
    Bitboard kb = pos.bb_side[s] & pos.bb_type[VUONG];
    return kb.any() ? kb.lsb() : -1;
}

int compute_evasion_targets(const Position& pos, Side stm, int own_king, bool target_mask[NUM_SQUARES], int& out_chk_sq) {
    std::memset(target_mask, 0, NUM_SQUARES);
    Side enemy = other_side(stm);
    int checkers[NUM_SQUARES];
    int nchk = get_attackers(pos, own_king, enemy, checkers);
    out_chk_sq = (nchk == 1) ? checkers[0] : -1;

    if (nchk == 1) {
        int csq = checkers[0];
        target_mask[csq] = true;
        const Piece atk = pos.top_piece[csq];
        if (atk.type == LONG || atk.type == PHUONG || atk.type == XA) {
            Bitboard btw = BB_BETWEEN[own_king][csq];
            while (btw.any()) {
                target_mask[btw.pop_lsb()] = true;
            }
        }
    }
    return nchk;
}

} // namespace

template <bool SortOutput>
void generate_checks_impl(Position& pos, MoveList& out, int8_t known_in_check = -1) {
    ensure_mate_init();
    out.clear();

    Side stm = static_cast<Side>(pos.side_to_move);
    Side enemy = other_side(stm);
    int eking = find_king_sq(pos, enemy);
    if (eking < 0) return;

    uint8_t base_f = initial_facing(stm);
    const Bitboard ek_ortho = ortho_attacks(eking, pos.bb_occ);
    const Bitboard ek_diag  = diag_attacks(eking, pos.bb_occ);

    if (pos.reserve[stm] != 0) {
        if (((ek_ortho | ek_diag) & BB_DROP_ROW[stm]).empty()) return;
        bool currently_in_check = (known_in_check >= 0) ? (known_in_check != 0) : is_in_check(pos, stm);
        for (uint8_t pt : {LONG, PHUONG}) {
            if (!pos.has_reserve_type(stm, pt)) continue;
            Bitboard hits = ((pt == LONG) ? ek_ortho : ek_diag) & BB_DROP_ROW[stm];
            while (hits.any()) {
                int dsq = hits.pop_lsb();
                uint8_t dh = pos.height[dsq];
                if (!(dh == 0 || (pos.cell_owner(dsq) == stm && pos.top_piece[dsq].type != VUONG && dh < MAX_STACK_SIZE))) {
                    continue;
                }
                Move m{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, static_cast<int8_t>(base_f), 0};
                if (!currently_in_check || check_move_leaves_king_safe(pos, m, true)) {
                    out.push(m);
                }
            }
        }
        return;
    }

    // Ultra-fast check if ANY prisoner or board piece could possibly deliver check
    uint8_t has_any_pris = 0;
    for (int pt = 1; pt < 8; ++pt) has_any_pris |= pos.prisoners[stm][pt];
    const Bitboard own_bb = pos.bb_side[stm];
    const Bitboard own_sliders = own_bb & (pos.bb_type[LONG] | pos.bb_type[PHUONG] | pos.bb_type[XA]);
    const Bitboard own_stacks = own_bb & pos.bb_stack;
    if (!has_any_pris && own_sliders.empty() && own_stacks.empty() &&
        (own_bb & BB_CHEB2[eking]).empty() &&
        (own_bb & pos.bb_type[KI] & BB_CHEB4[eking]).empty()) {
        return;
    }

    bool currently_in_check = (known_in_check >= 0) ? (known_in_check != 0) : is_in_check(pos, stm);

    if (has_any_pris) {
        constexpr uint8_t DROP_ORDER[7] = {TUONG, SI, KI, XA, LONG, PHUONG, BINH};
        for (int oi = 0; oi < 7; ++oi) {
            uint8_t pt = DROP_ORDER[oi];
            if (pos.prisoners[stm][pt] == 0) continue;

            auto try_drop = [&](int dsq, int8_t f) {
                uint8_t dh = pos.height[dsq];
                if (dh > 0) {
                    if (pos.cell_owner(dsq) != stm || pos.cell_contains_king(dsq) || dh >= MAX_STACK_SIZE) return;
                }
                Move m{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, f, 0};
                if (!currently_in_check || check_move_leaves_king_safe(pos, m, true)) {
                    out.push(m);
                }
            };

            if (pt == LONG) {
                Bitboard hits = ek_ortho & ~pos.bb_side[enemy];
                while (hits.any()) try_drop(hits.pop_lsb(), static_cast<int8_t>(base_f));
            } else if (pt == PHUONG) {
                Bitboard hits = ek_diag & ~pos.bb_side[enemy];
                while (hits.any()) try_drop(hits.pop_lsb(), static_cast<int8_t>(base_f));
            } else if (pt == XA) {
                for (int dir = 0; dir < 4; ++dir) {
                    // XA facing `dir` attacks along dir; so from eking, XA sits along opposite ray (dir ^ 2)
                    Bitboard hits = ek_ortho & BB_RAY_ORTHO[eking][dir ^ 2] & ~pos.bb_side[enemy];
                    while (hits.any()) try_drop(hits.pop_lsb(), static_cast<int8_t>(dir));
                }
            } else {
                const auto& rlist = g_rev_step[eking];
                uint8_t bit = static_cast<uint8_t>(1U << pt);
                for (uint8_t i = 0; i < rlist.count; ++i) {
                    const auto& e = rlist.entries[i];
                    uint8_t dh = pos.height[e.src_sq];
                    if (dh > 0) {
                        if (pos.cell_owner(e.src_sq) != stm || pos.cell_contains_king(e.src_sq) || dh >= MAX_STACK_SIZE) continue;
                    }
                    for (int8_t f = 0; f < 4; ++f) {
                        if (e.ptype_mask_by_facing[f] & bit) {
                            try_drop(e.src_sq, f);
                        }
                    }
                }
            }
        }
    }

    // Discovered check blockers via bitboard slider_blockers (only when own slider is aligned with enemy king!)
    Bitboard disc_bb;
    Bitboard aligned_snipers =
        (BB_ORTHO_MASK[eking] & own_bb & (pos.bb_type[LONG] | pos.bb_type[XA])) |
        (BB_DIAG_MASK[eking]  & own_bb & pos.bb_type[PHUONG]);
    while (aligned_snipers.any()) {
        int ssq = aligned_snipers.pop_lsb();
        const Piece sp = pos.top_piece[ssq];
        if (sp.type == XA && !BB_RAY_ORTHO[ssq][sp.facing].test_bit(eking)) continue;
        Bitboard btw = BB_BETWEEN[eking][ssq] & pos.bb_occ;
        if (btw.any() && !btw.more_than_one()) {
            disc_bb |= (btw & own_bb);
        }
    }

    Bitboard expose_bb;
    Bitboard st_bb = own_stacks;
    while (st_bb.any()) {
        int ssq = st_bb.pop_lsb();
        const Piece under = pos.cell_second(ssq);
        if (piece_attacks_sq(pos, ssq, under.type, under.facing, eking)) {
            expose_bb.set_bit(ssq);
        }
    }

    auto fast_attacks_eking = [&](int from_sq, uint8_t pt, uint8_t f, int ignore_sq) -> bool {
        if (pt == LONG) {
            if (ek_ortho.test_bit(from_sq)) return true;
            if (ignore_sq >= 0 && ek_ortho.test_bit(ignore_sq) && BB_ORTHO_MASK[eking].test_bit(from_sq)) {
                return (BB_BETWEEN[eking][from_sq] & (pos.bb_occ ^ Bitboard::from_sq(ignore_sq))).empty();
            }
            return false;
        }
        if (pt == PHUONG) {
            if (ek_diag.test_bit(from_sq)) return true;
            if (ignore_sq >= 0 && ek_diag.test_bit(ignore_sq) && BB_DIAG_MASK[eking].test_bit(from_sq)) {
                return (BB_BETWEEN[eking][from_sq] & (pos.bb_occ ^ Bitboard::from_sq(ignore_sq))).empty();
            }
            return false;
        }
        if (pt == XA) {
            if (!BB_RAY_ORTHO[from_sq][f].test_bit(eking)) return false;
            if (ek_ortho.test_bit(from_sq)) return true;
            if (ignore_sq >= 0 && ek_ortho.test_bit(ignore_sq)) {
                return (BB_BETWEEN[eking][from_sq] & (pos.bb_occ ^ Bitboard::from_sq(ignore_sq))).empty();
            }
            return false;
        }
        return BB_STEP_ATTACKS[pt][f][from_sq].test_bit(eking);
    };

    Bitboard pinners;
    bool pins_computed = false;
    Bitboard pin_blockers;

    constexpr uint8_t rots_rotatable[3] = {0, 1, 3};
    constexpr uint8_t rots_fixed[1] = {0};

    // Only visit candidate board pieces that can reach a check square or discover/expose check!
    Bitboard cand_pieces =
        disc_bb | expose_bb | own_sliders |
        (own_bb & BB_CHEB2[eking]) |
        (own_bb & pos.bb_type[KI] & BB_CHEB4[eking]);

    while (cand_pieces.any()) {
        int ssq = cand_pieces.pop_lsb();
        const Piece top = pos.top_piece[ssq];
        uint8_t sh = pos.height[ssq];
        bool is_stack = (sh > 1);
        bool src_has_king = pos.cell_contains_king(ssq);
        bool top_rot = can_rotate(top.type);
        const uint8_t* rots = top_rot ? rots_rotatable : rots_fixed;
        int num_rots = top_rot ? 3 : 1;

        bool can_disc = disc_bb.test_bit(ssq);
        bool can_exp = expose_bb.test_bit(ssq);

        Bitboard targets = piece_attacks_bb(pos, ssq, top.type, top.facing);
        if (!can_disc && !can_exp) {
            if (top.type == LONG || top.type == XA) {
                targets &= (ek_ortho.test_bit(ssq) ? BB_ORTHO_MASK[eking] : ek_ortho);
            } else if (top.type == PHUONG) {
                targets &= (ek_diag.test_bit(ssq) ? BB_DIAG_MASK[eking] : ek_diag);
            } else {
                targets &= BB_STEP_ZONE[eking];
            }
        }

        if (targets.any()) {
            if (!pins_computed && !currently_in_check) {
                pin_blockers = slider_blockers(pos, stm, pinners);
                pins_computed = true;
            }
            while (targets.any()) {
                int dsq = targets.pop_lsb();
                uint8_t dh = pos.height[dsq];
                bool is_own = (dh > 0 && pos.cell_owner(dsq) == stm);
                if (!is_own && dh > 0 && pos.cell_contains_king(dsq)) continue;

                bool can_single = !is_own;
                if (is_own) {
                    can_single = (!is_stack && top.type != VUONG && !pos.cell_contains_king(dsq) && dh < MAX_STACK_SIZE);
                }
                bool can_team = (is_stack && !is_own && !src_has_king);

                int8_t single_legal = -1;
                int8_t team_legal = -1;

                auto check_leg = [&](MoveKind k, int8_t& cached) -> bool {
                    if (cached >= 0) return cached == 1;
                    Move test{k, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, 0};
                    bool ok = currently_in_check
                        ? check_move_leaves_king_safe(pos, test, true)
                        : check_move_leaves_king_safe_pinned(pos, test, pin_blockers);
                    cached = ok ? 1 : 0;
                    return ok;
                };

                for (int ri = 0; ri < num_rots; ++ri) {
                    uint8_t rot = rots[ri];
                    uint8_t end_f = top_rot ? static_cast<uint8_t>((top.facing + rot) & 3) : top.facing;

                    if (can_single) {
                        bool single_empties_src = !is_stack;
                        bool direct = fast_attacks_eking(dsq, top.type, end_f, single_empties_src ? ssq : -1);
                        bool need_verify_check = false;
                        if (!direct) {
                            if (single_empties_src && can_disc) need_verify_check = true;
                            else if (!single_empties_src && can_exp) need_verify_check = true;
                        }
                        if (direct || need_verify_check) {
                            Move m{MoveKind::Single, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rot};
                            if (need_verify_check) {
                                UndoState u;
                                apply_move(pos, m, u);
                                bool leg = !is_in_check(pos, stm);
                                bool chk = leg && is_in_check(pos, enemy);
                                undo_move(pos, m, u);
                                single_legal = leg ? 1 : 0;
                                if (chk) out.push(m);
                            } else if (check_leg(MoveKind::Single, single_legal)) {
                                out.push(m);
                            }
                        }
                    }

                    if (can_team) {
                        bool direct = fast_attacks_eking(dsq, top.type, end_f, ssq);
                        bool need_verify_check = (!direct && can_disc);
                        if (direct || need_verify_check) {
                            Move m{MoveKind::Team, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rot};
                            if (need_verify_check) {
                                UndoState u;
                                apply_move(pos, m, u);
                                bool leg = !is_in_check(pos, stm);
                                bool chk = leg && is_in_check(pos, enemy);
                                undo_move(pos, m, u);
                                team_legal = leg ? 1 : 0;
                                if (chk) out.push(m);
                            } else if (check_leg(MoveKind::Team, team_legal)) {
                                out.push(m);
                            }
                        }
                    }
                }
            }
        }

        if (top_rot && (BB_STEP_ZONE[eking].test_bit(ssq) || (top.type == XA && ek_ortho.test_bit(ssq)))) {
            for (uint8_t rot : {uint8_t(1), uint8_t(3)}) {
                uint8_t end_f = static_cast<uint8_t>((top.facing + rot) & 3);
                if (fast_attacks_eking(ssq, top.type, end_f, -1)) {
                    Move m{MoveKind::Rotate, static_cast<int8_t>(ssq), -1, top.type, -1, rot};
                    if (!currently_in_check) {
                        out.push(m);
                    } else {
                        UndoState u;
                        apply_move(pos, m, u);
                        bool leg = !is_in_check(pos, stm);
                        undo_move(pos, m, u);
                        if (leg) out.push(m);
                    }
                }
            }
        }
    }

    if constexpr (SortOutput) {
        if (out.count > 1) {
            auto rel_sq = [stm](int sq) -> int {
                return (stm == CAM) ? (80 - sq) : sq;
            };
            auto check_key = [&](const Move& m) -> uint64_t {
                int is_non_drop = (m.kind == MoveKind::Drop) ? 0 : 1;
                int ptr = (m.kind == MoveKind::Drop) ? static_cast<int>(m.ptype) : 0;
                int rs = (m.src >= 0) ? rel_sq(m.src) : 81;
                int rd = (m.dst >= 0) ? rel_sq(m.dst) : 81;
                int f_val = (m.kind == MoveKind::Drop)
                    ? (can_rotate(m.ptype) ? ((stm == CAM) ? ((m.facing + 2) & 3) : (m.facing & 3)) : 0)
                    : static_cast<int>(m.rotate_after & 3);
                return (static_cast<uint64_t>(is_non_drop) << 40) |
                       (static_cast<uint64_t>(ptr) << 32) |
                       (static_cast<uint64_t>(rs) << 24) |
                       (static_cast<uint64_t>(rd) << 16) |
                       (static_cast<uint64_t>(static_cast<int>(m.kind)) << 8) |
                       static_cast<uint64_t>(f_val);
            };
            std::sort(out.moves, out.moves + out.count, [&](const Move& a, const Move& b) {
                return check_key(a) < check_key(b);
            });
        }
    }
}

void generate_checks(Position& pos, MoveList& out) {
    generate_checks_impl<true>(pos, out);
}

void generate_evasions(Position& pos, MoveList& out) {
    ensure_mate_init();
    out.clear();

    Side stm = static_cast<Side>(pos.side_to_move);
    int own_king = find_king_sq(pos, stm);
    if (own_king < 0) return;

    Side enemy = other_side(stm);
    if (!is_attacked(pos, own_king, enemy)) {
        generate_legal_moves(pos, out);
        return;
    }

    bool target_mask[NUM_SQUARES];
    int chk_sq = -1;
    int nchk = compute_evasion_targets(pos, stm, own_king, target_mask, chk_sq);
    uint8_t base_f = initial_facing(stm);

    if (pos.reserve[stm] != 0) {
        if (nchk >= 2) return;
        int drop_row = (stm == CAM) ? 1 : 7;
        for (uint8_t pt : {LONG, PHUONG}) {
            if (!pos.has_reserve_type(stm, pt)) continue;
            for (int c = 0; c < BOARD_SIZE; ++c) {
                int dsq = make_sq(drop_row, c);
                if (!target_mask[dsq]) continue;
                uint8_t dh = pos.height[dsq];
                if (!(dh == 0 || (pos.cell_owner(dsq) == stm && !pos.cell_contains_king(dsq) && dh < MAX_STACK_SIZE))) {
                    continue;
                }
                Move m{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, static_cast<int8_t>(base_f), 0};
                if (check_move_leaves_king_safe(pos, m, true)) out.push(m);
            }
        }
        return;
    }

    Bitboard pinners;
    Bitboard pin_blockers = slider_blockers(pos, stm, pinners);
    Bitboard occ_no_king = pos.bb_occ ^ Bitboard::from_sq(own_king);
    Bitboard target_bb;
    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        if (target_mask[sq]) target_bb.set_bit(sq);
    }

    constexpr uint8_t rots_rotatable[3] = {0, 1, 3};
    constexpr uint8_t rots_fixed[1] = {0};

    auto gen_sq_evasions = [&](int ssq, bool is_king_sq) {
        uint8_t sh = pos.height[ssq];
        if (sh == 0 || pos.cell_owner(ssq) != stm) return;
        if (nchk >= 2 && !is_king_sq) return;

        const Piece top = pos.top_piece[ssq];
        bool is_stack = (sh > 1);
        bool src_has_king = pos.cell_contains_king(ssq);
        bool top_rot = can_rotate(top.type);
        const uint8_t* rots = top_rot ? rots_rotatable : rots_fixed;
        int num_rots = top_rot ? 3 : 1;

        Bitboard dests = piece_attacks_bb(pos, ssq, top.type, top.facing) & ~pos.bb_side[stm];
        if (!is_king_sq) dests &= target_bb;

        bool is_pinned = (!is_king_sq && pin_blockers.test_bit(ssq));

        while (dests.any()) {
            int dsq = dests.pop_lsb();
            if (pos.height[dsq] > 0 && pos.cell_contains_king(dsq)) continue;

            bool single_leg = is_king_sq
                ? !is_attacked_with_occ(pos, dsq, enemy, occ_no_king)
                : (!is_pinned || is_stack);
            if (single_leg) {
                for (int ri = 0; ri < num_rots; ++ri) {
                    out.push(Move{MoveKind::Single, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rots[ri]});
                }
            }
            if (is_stack && !src_has_king && !is_pinned) {
                for (int ri = 0; ri < num_rots; ++ri) {
                    out.push(Move{MoveKind::Team, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rots[ri]});
                }
            }
        }
    };

    // Order: 1. King moves, 2. Non-king captures/blocks, 3. Prisoner drop interpositions
    gen_sq_evasions(own_king, true);
    if (nchk == 1) {
        Bitboard non_king_bb = pos.bb_side[stm];
        non_king_bb.clear_bit(own_king);
        while (non_king_bb.any()) {
            gen_sq_evasions(non_king_bb.pop_lsb(), false);
        }
        Bitboard empty_targets = target_bb & ~pos.bb_occ;
        if (empty_targets.any()) {
            for (uint8_t pt = 0; pt < 8; ++pt) {
                if (pos.prisoners[stm][pt] == 0) continue;
                bool rot = can_rotate(pt);
                Bitboard et = empty_targets;
                while (et.any()) {
                    int dsq = et.pop_lsb();
                    if (rot) {
                        for (int8_t f = 0; f < 4; ++f) {
                            out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, f, 0});
                        }
                    } else {
                        out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, static_cast<int8_t>(base_f), 0});
                    }
                }
            }
        }
    }

    if (out.count > 1) {
        auto rel_sq = [stm](int sq) -> int {
            return (stm == CAM) ? (80 - sq) : sq;
        };
        auto ev_key = [&](const Move& m) -> uint64_t {
            int grp = (m.kind != MoveKind::Drop && m.src == own_king) ? 0 : ((m.kind != MoveKind::Drop) ? 1 : 2);
            int is_non_cap = (m.dst < 0 || pos.height[m.dst] == 0 || pos.cell_owner(m.dst) == stm) ? 1 : 0;
            int rs = (m.src >= 0) ? rel_sq(m.src) : 81;
            int rd = (m.dst >= 0) ? rel_sq(m.dst) : 81;
            int f_val = (m.kind == MoveKind::Drop)
                ? (can_rotate(m.ptype) ? ((stm == CAM) ? ((m.facing + 2) & 3) : (m.facing & 3)) : 0)
                : static_cast<int>(m.rotate_after & 3);
            return (static_cast<uint64_t>(grp) << 40) |
                   (static_cast<uint64_t>(is_non_cap) << 36) |
                   (static_cast<uint64_t>(m.ptype) << 32) |
                   (static_cast<uint64_t>(rs) << 24) |
                   (static_cast<uint64_t>(rd) << 16) |
                   (static_cast<uint64_t>(static_cast<int>(m.kind)) << 8) |
                   static_cast<uint64_t>(f_val);
        };
        std::sort(out.moves, out.moves + out.count, [&](const Move& a, const Move& b) {
            return ev_key(a) < ev_key(b);
        });
    }
}

bool has_any_legal_evasion(Position& pos) {
    ensure_mate_init();
    Side stm = static_cast<Side>(pos.side_to_move);
    int own_king = find_king_sq(pos, stm);
    if (own_king < 0) return false;

    Side enemy = other_side(stm);
    if (!is_attacked(pos, own_king, enemy)) {
        MoveList legal;
        generate_legal_moves(pos, legal);
        return legal.count > 0;
    }

    bool target_mask[NUM_SQUARES];
    int chk_sq = -1;
    int nchk = compute_evasion_targets(pos, stm, own_king, target_mask, chk_sq);
    uint8_t base_f = initial_facing(stm);

    if (pos.reserve[stm] != 0) {
        if (nchk >= 2) return false;
        int drop_row = (stm == CAM) ? 1 : 7;
        for (uint8_t pt : {LONG, PHUONG}) {
            if (!pos.has_reserve_type(stm, pt)) continue;
            for (int c = 0; c < BOARD_SIZE; ++c) {
                int dsq = make_sq(drop_row, c);
                if (!target_mask[dsq]) continue;
                uint8_t dh = pos.height[dsq];
                if (!(dh == 0 || (pos.cell_owner(dsq) == stm && !pos.cell_contains_king(dsq) && dh < MAX_STACK_SIZE))) {
                    continue;
                }
                Move m{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, static_cast<int8_t>(base_f), 0};
                if (check_move_leaves_king_safe(pos, m, true)) return true;
            }
        }
        return false;
    }

    // 1. Check King escape moves via bitboard (zero apply_move!)
    Bitboard occ_no_king = pos.bb_occ ^ Bitboard::from_sq(own_king);
    Bitboard king_dests = BB_STEP_ATTACKS[VUONG][0][own_king] & ~pos.bb_side[stm];
    if (pos.king_sq[enemy] < NUM_SQUARES) king_dests.clear_bit(pos.king_sq[enemy]);
    while (king_dests.any()) {
        int dsq = king_dests.pop_lsb();
        if (!is_attacked_with_occ(pos, dsq, enemy, occ_no_king)) return true;
    }

    if (nchk >= 2) return false;

    Bitboard target_bb;
    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        if (target_mask[sq]) target_bb.set_bit(sq);
    }

    // 2. Check drop interposition on empty square between slider checker and King
    uint8_t chk_pt = pos.top_piece[chk_sq].type;
    if (chk_pt == LONG || chk_pt == PHUONG || chk_pt == XA) {
        uint8_t has_prisoner = 0;
        for (uint8_t pt = 0; pt < 8; ++pt) has_prisoner |= pos.prisoners[stm][pt];
        if (has_prisoner != 0 && (BB_BETWEEN[own_king][chk_sq] & ~pos.bb_occ).any()) {
            return true;
        }
    }

    // 3. Check non-king captures or blocks via bitboard + pin_blockers (zero apply_move!)
    Bitboard pinners;
    Bitboard pin_blockers = slider_blockers(pos, stm, pinners);
    Bitboard non_king_bb = pos.bb_side[stm];
    non_king_bb.clear_bit(own_king);
    while (non_king_bb.any()) {
        int ssq = non_king_bb.pop_lsb();
        if (pin_blockers.test_bit(ssq) && pos.height[ssq] == 1) continue;
        const Piece top = pos.top_piece[ssq];
        Bitboard hits = piece_attacks_bb(pos, ssq, top.type, top.facing) & target_bb & ~pos.bb_side[stm];
        if (pos.king_sq[enemy] < NUM_SQUARES) hits.clear_bit(pos.king_sq[enemy]);
        if (hits.any()) return true;
    }
    return false;
}

namespace {

// Monotone df-pn Transposition Table keyed by (zobrist, is_or) without rem in key
struct DfpnTTEntry {
    uint64_t key = 0;
    uint32_t pn = 1;
    uint32_t dn = 1;
    Move best_move = Move::none();
    uint8_t rem = 0;
    uint8_t proved_len = 255;    // 255 = unproved, else shortest proved mate length
    int8_t disproved_rem = -1;   // -1 = not disproved, else max rem proved non-mating
    uint8_t _pad = 0;
    uint16_t gen = 0;
};

constexpr size_t DFPN_TT_SIZE = 262144; // 2^18 buckets x 2 slots (~16 MB reusable thread_local)
struct DfpnTTBucket {
    DfpnTTEntry slot[2];
};

struct AnalyzeCacheEntry {
    uint64_t key = 0;
    uint16_t est = 0;
    uint8_t is_mate = 0;
    uint8_t valid = 0;
    uint32_t _pad = 0;
};
constexpr size_t ANALYZE_CACHE_SIZE = 262144; // 4 MB thread-local leaf cache

static constexpr int MAX_MATE_DEPTH = 42;

struct DfpnNodeMoves {
    MoveList list;
    uint32_t pn[MAX_MOVES];
    uint32_t dn[MAX_MOVES];
    uint8_t mate_len[MAX_MOVES];
    uint8_t rep_dep[MAX_MOVES];
};

struct DfpnThreadStorage {
    std::vector<DfpnTTBucket> tt;
    std::vector<AnalyzeCacheEntry> ac;
    std::vector<DfpnNodeMoves> stack;
    uint16_t gen = 0;
    size_t proved_count = 0;
    bool last_had_rep2 = false;

    DfpnThreadStorage()
        : tt(DFPN_TT_SIZE), ac(ANALYZE_CACHE_SIZE), stack(MAX_MATE_DEPTH + 6) {}

    void invalidate() {
        ++gen;
        if (gen == 0) {
            std::memset(tt.data(), 0, tt.size() * sizeof(DfpnTTBucket));
            gen = 1;
        }
        proved_count = 0;
        std::memset(ac.data(), 0, ac.size() * sizeof(AnalyzeCacheEntry));
        last_had_rep2 = false;
    }
};

DfpnThreadStorage& get_dfpn_storage() {
    static thread_local DfpnThreadStorage s_store;
    return s_store;
}

class DfpnSolver {
public:
    DfpnSolver(
        int64_t time_budget_ms,
        uint64_t max_nodes,
        const std::atomic<bool>* stop_flag,
        const std::vector<RepHashEntry>* base_rep,
        const uint64_t* prior_path = nullptr,
        int prior_ply = 0,
        bool reuse_tt = true,
        const Move* excluded_moves = nullptr,
        int num_excluded = 0,
        std::atomic<uint64_t>* live_nodes_counter = nullptr
    )
        : start_ms_(now_ms()),
          time_budget_ms_(time_budget_ms),
          max_nodes_(max_nodes),
          stop_flag_(stop_flag),
          base_rep_(base_rep),
          prior_path_(prior_path),
          prior_ply_(prior_ply),
          excluded_moves_(excluded_moves),
          num_excluded_(num_excluded),
          live_nodes_counter_(live_nodes_counter) {
        DfpnThreadStorage& st = get_dfpn_storage();
        tt_ = &st.tt;
        ac_ = &st.ac;
        stack_ = &st.stack;

        bool has_rep2_base = false;
        if (base_rep_) {
            for (const auto& e : *base_rep_) {
                if (e.count >= 2) {
                    has_rep2_base = true;
                    break;
                }
            }
        }
        if (st.gen == 0 || !reuse_tt || has_rep2_base || st.last_had_rep2 ||
            st.proved_count >= (DFPN_TT_SIZE / 4)) {
            ++st.gen;
            if (st.gen == 0) {
                std::memset(st.tt.data(), 0, st.tt.size() * sizeof(DfpnTTBucket));
                st.gen = 1;
            }
            st.proved_count = 0;
        }
        st.last_had_rep2 = has_rep2_base;
        gen_ = st.gen;
        proved_count_ = &st.proved_count;
    }

    MateResult solve(const Position& root_pos, int max_ply, bool find_shortest = true) {
        MateResult res;
        Position pos = root_pos;
        max_ply = std::min(max_ply, MAX_MATE_DEPTH);
        if ((max_ply & 1) == 0 && max_ply > 1) {
            max_ply -= 1;
        }
        path_hashes_[0] = pos.zobrist;
        root_in_base_rep_ = false;
        if (base_rep_ && prior_ply_ == 0) {
            for (const auto& e : *base_rep_) {
                if (e.hash == pos.zobrist) {
                    root_in_base_rep_ = true;
                    break;
                }
            }
        }

        // Ascending schedule so short/mid mates resolve before deep exploration.
        int limits[16];
        int num_limits = 0;
        static const int STEPS[] = {1, 3, 5, 7, 13, 15, 23, 31};
        for (int s : STEPS) {
            if (s < max_ply) {
                limits[num_limits++] = s;
            }
        }
        limits[num_limits++] = max_ply;

        const uint64_t orig_max_nodes = max_nodes_;
        const int64_t orig_budget_ms = time_budget_ms_;
        uint32_t last_pn = PN_INF, last_dn = PN_INF;
        int highest_disproved_lim = 0;

        for (int li = 0; li < num_limits; ++li) {
            int lim = limits[li];
            if (check_abort()) break;
            bool is_intermediate = (lim < max_ply);
            if (is_intermediate) {
                uint64_t step_cap = orig_max_nodes / 2;
                if (lim <= 3) step_cap = std::min<uint64_t>(12000ULL, orig_max_nodes / 16);
                else if (lim == 5) step_cap = std::min<uint64_t>(20000ULL, orig_max_nodes / 12);
                else if (lim == 7) step_cap = std::min<uint64_t>(45000ULL, orig_max_nodes / 10);
                else if (lim == 13) step_cap = std::min<uint64_t>(360000ULL, orig_max_nodes / 2);
                else if (lim <= 15) step_cap = std::min<uint64_t>(300000ULL, orig_max_nodes / 3);
                else if (lim <= 23) step_cap = std::min<uint64_t>(300000ULL, orig_max_nodes / 3);
                else if (lim <= 31) step_cap = std::min<uint64_t>(300000ULL, orig_max_nodes / 3);
                step_cap = std::max<uint64_t>(step_cap, 2000ULL);
                max_nodes_ = std::min(orig_max_nodes, nodes_ + step_cap);
            } else {
                max_nodes_ = orig_max_nodes;
            }

            uint32_t pn = PN_INF, dn = PN_INF;
            uint8_t mate_len = 0;
            Move best_m = Move::none();
            bool rep_dep = false;
            dfpn(pos, lim, 0, true, PN_INF, PN_INF, pn, dn, best_m, mate_len, rep_dep);
            last_pn = pn;
            last_dn = dn;
            bool step_aborted = abort_;

            if (is_intermediate) {
                max_nodes_ = orig_max_nodes;
                abort_ = false;
                if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) abort_ = true;
                if (time_budget_ms_ > 0 && (now_ms() - start_ms_) >= time_budget_ms_) abort_ = true;
            }

            if (pn == 0) {
                std::vector<Move> cand_pv;
                bool pv_rep_dep = false;
                int target_rem = (mate_len > 0 && static_cast<int>(mate_len) < lim)
                    ? static_cast<int>(mate_len)
                    : lim;
                bool pv_ok = extract_pv(root_pos, target_rem, cand_pv, best_m, pv_rep_dep, false);
                if (!pv_ok && target_rem < lim) {
                    cand_pv.clear();
                    pv_rep_dep = false;
                    pv_ok = extract_pv(root_pos, lim, cand_pv, best_m, pv_rep_dep, false);
                }
                if (!pv_ok) {
                    cand_pv.clear();
                    pv_rep_dep = false;
                    pv_ok = extract_pv(root_pos, lim, cand_pv, best_m, pv_rep_dep, true);
                }
                if (!pv_ok || cand_pv.empty()) {
                    time_budget_ms_ = orig_budget_ms;
                    max_nodes_ = orig_max_nodes;
                    if (dirty_rep_tt_) get_dfpn_storage().last_had_rep2 = true;
                    res.found = false;
                    res.aborted = true;
                    res.pv.clear();
                    res.nodes = nodes_;
                    res.root_pn = 1;
                    res.root_dn = 1;
                    return res;
                }

                res.found = true;
                res.rep_dependent = rep_dep || pv_rep_dep;
                res.root_pn = 0;
                res.root_dn = PN_INF;
                res.pv = std::move(cand_pv);
                res.mate_ply = static_cast<int>(res.pv.size());
                // Only declare proved_shortest from df-pn's sound proof bound (never from an unverified shortened PV).
                if (lim <= highest_disproved_lim + 2 ||
                    (mate_len > 0 && static_cast<int>(mate_len) <= highest_disproved_lim + 2)) {
                    res.proved_shortest = true;
                }

                // Shrink-to-Shortest loop: tighten odd depth limit strictly within wall-clock deadline.
                if (find_shortest && !res.proved_shortest && res.mate_ply > 1) {
                    int64_t shrink_deadline_ms = orig_budget_ms;
                    time_budget_ms_ = shrink_deadline_ms;
                    max_nodes_ = (orig_max_nodes <= 10000ULL)
                        ? orig_max_nodes
                        : std::max(orig_max_nodes, nodes_ + 250000ULL);

                    for (int q_lim : {5, 7, 9, 13}) {
                        if (q_lim >= res.mate_ply - 2 || q_lim <= highest_disproved_lim) continue;
                        if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) break;
                        if (time_budget_ms_ > 0 && (now_ms() - start_ms_) >= time_budget_ms_) break;
                        if (nodes_ >= max_nodes_) break;

                        uint64_t q_cap = (q_lim == 5) ? 25000ULL
                            : ((q_lim == 7) ? 80000ULL
                            : ((q_lim == 9) ? 90000ULL : 240000ULL));
                        abort_ = false;
                        uint32_t qpn = PN_INF, qdn = PN_INF;
                        uint8_t qml = 0;
                        Move qbm = Move::none();
                        bool qrd = false;
                        uint64_t saved_cap = max_nodes_;
                        max_nodes_ = std::min(saved_cap, nodes_ + q_cap);
                        dfpn(pos, q_lim, 0, true, PN_INF, PN_INF, qpn, qdn, qbm, qml, qrd);
                        max_nodes_ = saved_cap;
                        bool q_aborted = abort_;
                        abort_ = false;
                        if (qpn == 0) {
                            std::vector<Move> qpv;
                            bool qpv_rd = false;
                            int qtarget = (qml > 0 && static_cast<int>(qml) < q_lim) ? static_cast<int>(qml) : q_lim;
                            bool qpv_ok = extract_pv(root_pos, qtarget, qpv, qbm, qpv_rd, false);
                            if (!qpv_ok && qtarget < q_lim) {
                                qpv.clear();
                                qpv_rd = false;
                                qpv_ok = extract_pv(root_pos, q_lim, qpv, qbm, qpv_rd, false);
                            }
                            if (qpv_ok && !qpv.empty() && static_cast<int>(qpv.size()) < res.mate_ply) {
                                res.pv = std::move(qpv);
                                res.mate_ply = static_cast<int>(res.pv.size());
                                res.rep_dependent = qrd || qpv_rd;
                                if (res.mate_ply <= highest_disproved_lim + 2) {
                                    res.proved_shortest = true;
                                    break;
                                }
                            }
                        } else if (qdn == 0 && !q_aborted) {
                            highest_disproved_lim = std::max(highest_disproved_lim, q_lim);
                        }
                    }

                    while (res.mate_ply > 1) {
                        int shrink_lim = (res.mate_ply & 1) ? (res.mate_ply - 2) : (res.mate_ply - 1);
                        if (shrink_lim <= highest_disproved_lim) {
                            res.proved_shortest = true;
                            break;
                        }
                        if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) break;
                        if (time_budget_ms_ > 0 && (now_ms() - start_ms_) >= time_budget_ms_) break;
                        if (nodes_ >= max_nodes_) break;

                        const Move cur_bm = res.pv.front();
                        if (!cur_bm.is_none() && shrink_lim >= 3) {
                            UndoState u;
                            apply_move(pos, cur_bm, u);
                            if (count_reps(pos.zobrist, 1) < 3) {
                                path_hashes_[1] = pos.zobrist;
                                uint64_t saved_cap = max_nodes_;
                                max_nodes_ = std::min(saved_cap, nodes_ + 400000ULL);
                                abort_ = false;
                                uint32_t hpn = PN_INF, hdn = PN_INF;
                                Move hbm = Move::none();
                                uint8_t hml = 0;
                                bool hrd = false;
                                dfpn(pos, shrink_lim - 1, 1, false, PN_INF, PN_INF, hpn, hdn, hbm, hml, hrd);
                                max_nodes_ = saved_cap;
                                undo_move(pos, cur_bm, u);
                                bool h_aborted = abort_;
                                abort_ = false;
                                if (!h_aborted && hpn == 0) {
                                    std::vector<Move> hpv;
                                    bool hpv_rd = false;
                                    int htarget = (hml > 0 && static_cast<int>(hml + 1) < shrink_lim)
                                        ? static_cast<int>(hml + 1)
                                        : shrink_lim;
                                    bool hpv_ok = extract_pv(root_pos, htarget, hpv, cur_bm, hpv_rd, false);
                                    if (!hpv_ok && htarget < shrink_lim) {
                                        hpv.clear();
                                        hpv_rd = false;
                                        hpv_ok = extract_pv(root_pos, shrink_lim, hpv, cur_bm, hpv_rd, false);
                                    }
                                    if (hpv_ok && !hpv.empty() && static_cast<int>(hpv.size()) < res.mate_ply) {
                                        res.pv = std::move(hpv);
                                        res.mate_ply = static_cast<int>(res.pv.size());
                                        res.rep_dependent = hrd || hpv_rd;
                                        if (num_excluded_ == 0 && !res.rep_dependent) {
                                            store_tt(root_pos.zobrist, shrink_lim, true, 0, PN_INF, cur_bm, static_cast<uint8_t>(res.mate_ply));
                                        }
                                        continue;
                                    }
                                }
                            } else {
                                undo_move(pos, cur_bm, u);
                            }
                        }
                        if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) break;
                        if (time_budget_ms_ > 0 && (now_ms() - start_ms_) >= time_budget_ms_) break;
                        if (nodes_ >= max_nodes_) break;

                        abort_ = false;
                        uint32_t spn = PN_INF, sdn = PN_INF;
                        uint8_t smate_len = 0;
                        Move sbest_m = Move::none();
                        bool srep_dep = false;
                        dfpn(pos, shrink_lim, 0, true, PN_INF, PN_INF, spn, sdn, sbest_m, smate_len, srep_dep);
                        last_pn = spn;
                        last_dn = sdn;

                        if (spn == 0) {
                            std::vector<Move> spv;
                            bool spv_rd = false;
                            int starget = (smate_len > 0 && static_cast<int>(smate_len) < shrink_lim)
                                ? static_cast<int>(smate_len)
                                : shrink_lim;
                            bool spv_ok = extract_pv(root_pos, starget, spv, sbest_m, spv_rd, false);
                            if (!spv_ok && starget < shrink_lim) {
                                spv.clear();
                                spv_rd = false;
                                spv_ok = extract_pv(root_pos, shrink_lim, spv, sbest_m, spv_rd, false);
                            }
                            if (spv_ok && !spv.empty() && static_cast<int>(spv.size()) < res.mate_ply) {
                                res.pv = std::move(spv);
                                res.mate_ply = static_cast<int>(res.pv.size());
                                res.rep_dependent = srep_dep || spv_rd;
                            } else {
                                break;
                            }
                        } else if (sdn == 0 && !abort_) {
                            res.proved_shortest = true;
                            highest_disproved_lim = std::max(highest_disproved_lim, shrink_lim);
                            break;
                        } else {
                            break;
                        }
                    }
                }

                time_budget_ms_ = orig_budget_ms;
                max_nodes_ = orig_max_nodes;
                if (dirty_rep_tt_) get_dfpn_storage().last_had_rep2 = true;
                res.nodes = nodes_;
                return res;
            }

            if (dn == 0 && !step_aborted) {
                highest_disproved_lim = std::max(highest_disproved_lim, lim);
                if (lim >= max_ply) {
                    if (dirty_rep_tt_) get_dfpn_storage().last_had_rep2 = true;
                    res.found = false;
                    res.aborted = false;
                    res.nodes = nodes_;
                    res.root_pn = pn;
                    res.root_dn = dn;
                    return res;
                }
            }
        }
        max_nodes_ = orig_max_nodes;
        if (dirty_rep_tt_) get_dfpn_storage().last_had_rep2 = true;

        res.found = false;
        res.aborted = abort_;
        res.nodes = nodes_;
        res.root_pn = last_pn;
        res.root_dn = last_dn;
        return res;
    }

private:
    std::vector<DfpnTTBucket>* tt_ = nullptr;
    std::vector<AnalyzeCacheEntry>* ac_ = nullptr;
    std::vector<DfpnNodeMoves>* stack_ = nullptr;
    uint16_t gen_ = 1;
    size_t* proved_count_ = nullptr;
    bool dirty_rep_tt_ = false;
    bool root_in_base_rep_ = false;
    int64_t start_ms_ = 0;
    int64_t time_budget_ms_ = 80;
    uint64_t max_nodes_ = 5000000ULL;
    uint64_t nodes_ = 0;
    bool abort_ = false;
    const std::atomic<bool>* stop_flag_ = nullptr;
    const std::vector<RepHashEntry>* base_rep_ = nullptr;
    const uint64_t* prior_path_ = nullptr;
    int prior_ply_ = 0;
    const Move* excluded_moves_ = nullptr;
    int num_excluded_ = 0;
    std::atomic<uint64_t>* live_nodes_counter_ = nullptr;
    uint64_t last_reported_nodes_ = 0;

    uint64_t path_hashes_[MAX_MATE_DEPTH + 6]{};

    bool check_abort() {
        if (abort_) return true;
        if (nodes_ >= max_nodes_) {
            abort_ = true;
            return true;
        }
        if ((nodes_ & 255ULL) == 0) {
            if (live_nodes_counter_) {
                uint64_t delta = nodes_ - last_reported_nodes_;
                if (delta > 0) {
                    live_nodes_counter_->fetch_add(delta, std::memory_order_relaxed);
                    last_reported_nodes_ = nodes_;
                }
            }
            if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) {
                abort_ = true;
                return true;
            }
            if (time_budget_ms_ > 0 && (now_ms() - start_ms_) >= time_budget_ms_) {
                abort_ = true;
                return true;
            }
        }
        return false;
    }

    int count_reps(uint64_t hash, int ply) const {
        int cnt = 1;
        if (base_rep_) {
            for (const auto& e : *base_rep_) {
                if (e.hash == hash) {
                    cnt += e.count;
                    break;
                }
            }
        }
        if (prior_path_ && prior_ply_ > 0) {
            // prior_path_[0] is game root (already included in base_rep_ if base_rep_ != nullptr).
            // prior_path_[1 .. prior_ply_ - 1] are search nodes strictly before path_hashes_[0].
            int start_p = base_rep_ ? 1 : 0;
            for (int i = start_p; i < prior_ply_; ++i) {
                if (prior_path_[i] == hash && prior_path_[i] != 0ULL) ++cnt;
            }
            // path_hashes_[0] is the df-pn root at prior_ply_, NOT in prior_path_[0..prior_ply_-1].
            for (int i = 0; i < ply; ++i) {
                if (path_hashes_[i] == hash) ++cnt;
            }
        } else {
            // Root df-pn: skip path_hashes_[0] only if it was already counted in base_rep_.
            int start_i = root_in_base_rep_ ? 1 : 0;
            for (int i = start_i; i < ply; ++i) {
                if (path_hashes_[i] == hash) ++cnt;
            }
        }
        return cnt;
    }

    static uint64_t make_tt_key(uint64_t hash, bool is_or) {
        return hash ^ (is_or ? 0xBF58476D1CE4E5B9ULL : 0ULL);
    }

    bool lookup_tt(uint64_t hash, int rem, bool is_or, uint32_t& pn, uint32_t& dn, Move& best_m, uint8_t& mate_len) const {
        uint64_t k = make_tt_key(hash, is_or);
        const auto& b = (*tt_)[k & (DFPN_TT_SIZE - 1)];
        for (int i = 0; i < 2; ++i) {
            const auto& e = b.slot[i];
            if (e.gen == gen_ && e.key == k) {
                if (e.proved_len != 255 && static_cast<int>(e.proved_len) <= rem) {
                    pn = 0;
                    dn = PN_INF;
                    best_m = e.best_move;
                    mate_len = e.proved_len;
                    return true;
                }
                if (rem <= static_cast<int>(e.disproved_rem)) {
                    pn = PN_INF;
                    dn = 0;
                    best_m = Move::none();
                    mate_len = 0;
                    return true;
                }
                if (static_cast<int>(e.rem) == rem) {
                    pn = e.pn;
                    dn = e.dn;
                    best_m = e.best_move;
                    mate_len = 0;
                    return true;
                }
            }
        }
        return false;
    }

    void store_tt(uint64_t hash, int rem, bool is_or, uint32_t pn, uint32_t dn, Move best_m, uint8_t mate_len) {
        uint64_t k = make_tt_key(hash, is_or);
        auto& b = (*tt_)[k & (DFPN_TT_SIZE - 1)];
        for (int i = 0; i < 2; ++i) {
            auto& e = b.slot[i];
            if (e.gen == gen_ && e.key == k) {
                if (pn == 0) {
                    if (e.proved_len == 255 && proved_count_) {
                        ++(*proved_count_);
                    }
                    if (mate_len < e.proved_len || e.best_move.is_none()) {
                        if (mate_len < e.proved_len) e.proved_len = mate_len;
                        if (!best_m.is_none()) e.best_move = best_m;
                    }
                    if (static_cast<int>(e.rem) == rem) {
                        e.pn = 0;
                        e.dn = PN_INF;
                    }
                    return;
                }
                if (dn == 0) {
                    if (rem > static_cast<int>(e.disproved_rem)) {
                        e.disproved_rem = static_cast<int8_t>(rem);
                    }
                    if (static_cast<int>(e.rem) == rem) {
                        e.pn = PN_INF;
                        e.dn = 0;
                    }
                    return;
                }
                e.pn = pn;
                e.dn = dn;
                e.rem = static_cast<uint8_t>(rem);
                return;
            }
        }
        int dst = -1;
        if (b.slot[0].gen != gen_) dst = 0;
        else if (b.slot[1].gen != gen_) dst = 1;
        else if (pn == 0) dst = (b.slot[0].proved_len == 255) ? 0 : 1;
        else if (b.slot[1].proved_len == 255) dst = 1;
        else if (b.slot[0].proved_len == 255) dst = 0;
        else dst = 1;

        DfpnTTEntry ne;
        ne.key = k;
        ne.pn = pn;
        ne.dn = dn;
        ne.best_move = (pn == 0) ? best_m : Move::none();
        ne.rem = static_cast<uint8_t>(rem);
        ne.proved_len = (pn == 0) ? mate_len : 255;
        ne.disproved_rem = (dn == 0) ? static_cast<int8_t>(rem) : -1;
        ne.gen = gen_;
        bool replaced_was_proved = (b.slot[dst].gen == gen_ && b.slot[dst].proved_len != 255);
        if (pn == 0 && !replaced_was_proved && proved_count_) {
            ++(*proved_count_);
        }
        b.slot[dst] = ne;
    }

    // Analyzes defender position right after a check: checks for immediate mate
    // and estimates evasion multiplicity accurately (including drop interpositions).
    void analyze_after_check(Position& pos, bool& out_is_mate, int& out_est) {
        AnalyzeCacheEntry& ac = (*ac_)[pos.zobrist & (ANALYZE_CACHE_SIZE - 1)];
        if (ac.valid && ac.key == pos.zobrist) {
            out_is_mate = (ac.is_mate != 0);
            out_est = static_cast<int>(ac.est);
            return;
        }
        Side stm = static_cast<Side>(pos.side_to_move);
        Side atk = other_side(stm);
        int own_king = find_king_sq(pos, stm);
        if (own_king < 0) {
            out_is_mate = true;
            out_est = 0;
            ac = AnalyzeCacheEntry{pos.zobrist, 0, 1, 1, 0};
            return;
        }
        int checkers[NUM_SQUARES];
        int nchk = get_attackers(pos, own_king, atk, checkers);
        int chk_sq = (nchk == 1) ? checkers[0] : -1;

        int empty_ray = 0;
        bool has_interpose_drop = false;
        int slider_penalty = 0;
        bool check_dist_penalty = false;
        if (nchk == 1 && chk_sq >= 0) {
            const uint8_t ct = pos.top_piece[chk_sq].type;
            const bool is_slider = (ct == LONG || ct == PHUONG || ct == XA);
            int r0 = sq_row(own_king);
            int c0 = sq_col(own_king);
            int r1 = sq_row(chk_sq);
            int c1 = sq_col(chk_sq);
            int dr = r1 - r0;
            int dc = c1 - c0;
            int dist = std::max(std::abs(dr), std::abs(dc));
            if (dist > 1 && (dr == 0 || dc == 0 || std::abs(dr) == std::abs(dc))) {
                empty_ray = (BB_BETWEEN[own_king][chk_sq] & ~pos.bb_occ).popcount();
                if (empty_ray > 0) {
                    int ptypes = 0;
                    for (int pt = 0; pt < 8; ++pt) {
                        if (pos.prisoners[stm][pt] > 0) ++ptypes;
                    }
                    if (ptypes > 0) {
                        slider_penalty = empty_ray * ptypes * 3;
                        if (is_slider && pos.reserve[stm] == 0) {
                            has_interpose_drop = true;
                            if (empty_ray >= 1) {
                                out_is_mate = false;
                                out_est = slider_penalty + 2;
                                ac = AnalyzeCacheEntry{pos.zobrist, static_cast<uint16_t>(std::min(out_est, 65535)), 0, 1, 0};
                                return;
                            }
                        }
                    }
                }
                if (!is_slider) empty_ray = 0;
            }
            check_dist_penalty = true;
        }

        Bitboard kd = BB_STEP_ATTACKS[VUONG][0][own_king] & ~pos.bb_side[stm];
        if (pos.king_sq[atk] < NUM_SQUARES) kd.clear_bit(pos.king_sq[atk]);
        int k_unattacked = 0;
        while (kd.any()) {
            int d = kd.pop_lsb();
            if (!local_is_attacked(pos, d, atk)) {
                ++k_unattacked;
            }
        }
        int est = k_unattacked + slider_penalty;
        bool chk_attacked = false;
        if (check_dist_penalty) {
            chk_attacked = local_is_attacked(pos, chk_sq, stm);
            if (chk_attacked) ++est;
        }
        // Fast exact checkmate / survival resolution without redundant scans:
        // 1) At most `nchk` unattacked king destinations can suffer x-ray check -> >nchk guarantees survival.
        // 2) A legal prisoner drop between king and sole sliding checker guarantees survival.
        // 3) If k_unattacked == 0 and neither interposition nor checker capture exists, it's immediate mate.
        if (k_unattacked <= nchk && !has_interpose_drop) {
            bool known_mate = (k_unattacked == 0 && (nchk >= 2 || (nchk == 1 && empty_ray == 0 && !chk_attacked)));
            if (known_mate || !has_any_legal_evasion(pos)) {
                out_is_mate = true;
                out_est = 0;
                ac = AnalyzeCacheEntry{pos.zobrist, 0, 1, 1, 0};
                return;
            }
        }
        out_is_mate = false;
        out_est = std::max(1, est);
        ac = AnalyzeCacheEntry{pos.zobrist, static_cast<uint16_t>(std::min(out_est, 65535)), 0, 1, 0};
    }

    bool is_root_excluded(const Move& m) const {
        for (int i = 0; i < num_excluded_; ++i) {
            if (excluded_moves_[i] == m) return true;
        }
        return false;
    }

    void dfpn(
        Position& pos,
        int rem,
        int ply,
        bool is_or,
        uint32_t th_pn,
        uint32_t th_dn,
        uint32_t& out_pn,
        uint32_t& out_dn,
        Move& out_best,
        uint8_t& out_mate_len,
        bool& out_rep_dep
    ) {
        out_rep_dep = false;
        if (ply >= MAX_MATE_DEPTH + 4) {
            out_pn = PN_INF;
            out_dn = 0;
            return;
        }
        if (check_abort()) {
            out_pn = 1;
            out_dn = 1;
            return;
        }
        ++nodes_;

        const bool skip_root_tt = (ply == 0 && num_excluded_ > 0);

        // Probe TT for current node (skipped at root when root moves are excluded)
        if (!skip_root_tt) {
            uint32_t tpn = 1, tdn = 1;
            Move tbm = Move::none();
            uint8_t tml = 0;
            if (lookup_tt(pos.zobrist, rem, is_or, tpn, tdn, tbm, tml)) {
                if (tpn >= th_pn || tdn >= th_dn || tpn == 0 || tdn == 0) {
                    out_pn = tpn;
                    out_dn = tdn;
                    out_best = tbm;
                    out_mate_len = tml;
                    out_rep_dep = false;
                    return;
                }
            }
        }

        if (is_or) {
            if (rem <= 0) {
                out_pn = PN_INF;
                out_dn = 0;
                if (!skip_root_tt) {
                    store_tt(pos.zobrist, rem, true, out_pn, out_dn, Move::none(), 0);
                }
                return;
            }

            DfpnNodeMoves& nm = (*stack_)[ply];
            generate_checks(pos, nm.list);
            if (skip_root_tt && nm.list.count > 0) {
                int w = 0;
                for (int i = 0; i < nm.list.count; ++i) {
                    if (!is_root_excluded(nm.list.moves[i])) {
                        nm.list.moves[w++] = nm.list.moves[i];
                    }
                }
                nm.list.count = w;
            }
            int n = nm.list.count;
            if (n == 0) {
                out_pn = PN_INF;
                out_dn = 0;
                if (!skip_root_tt) {
                    store_tt(pos.zobrist, rem, true, out_pn, out_dn, Move::none(), 0);
                }
                return;
            }

            // Fast bottom leaf shortcut (rem == 1): only immediate checkmate works!
            if (rem == 1) {
                bool any_rep_chk = false;
                for (int i = 0; i < n; ++i) {
                    const Move m = nm.list.moves[i];
                    UndoState u;
                    apply_move(pos, m, u);
                    ++nodes_;
                    if (count_reps(pos.zobrist, ply + 1) >= 3) {
                        undo_move(pos, m, u);
                        any_rep_chk = true;
                        continue;
                    }
                    bool has_ev = has_any_legal_evasion(pos);
                    undo_move(pos, m, u);
                    if (!has_ev) {
                        out_pn = 0;
                        out_dn = PN_INF;
                        out_best = m;
                        out_mate_len = 1;
                        out_rep_dep = false;
                        if (!skip_root_tt) {
                            store_tt(pos.zobrist, rem, true, 0, PN_INF, m, 1);
                        }
                        return;
                    }
                }
                out_pn = PN_INF;
                out_dn = 0;
                out_rep_dep = any_rep_chk;
                if (out_rep_dep) dirty_rep_tt_ = true;
                if (!skip_root_tt && !out_rep_dep) {
                    store_tt(pos.zobrist, rem, true, PN_INF, 0, Move::none(), 0);
                }
                return;
            }

            // Evaluate / look up all children once on entry
            bool any_rep_child = false;
            for (int i = 0; i < n; ++i) {
                const Move m = nm.list.moves[i];
                UndoState u;
                apply_move(pos, m, u);
                if (count_reps(pos.zobrist, ply + 1) >= 3) {
                    undo_move(pos, m, u);
                    nm.pn[i] = PN_INF;
                    nm.dn[i] = 0;
                    nm.mate_len[i] = 0;
                    nm.rep_dep[i] = 1;
                    any_rep_child = true;
                    continue;
                }
                uint32_t cpn = 1, cdn = 1;
                Move cbm = Move::none();
                uint8_t cml = 0;
                if (!lookup_tt(pos.zobrist, rem - 1, false, cpn, cdn, cbm, cml)) {
                    bool is_mate = false;
                    int est = 1;
                    analyze_after_check(pos, is_mate, est);
                    if (is_mate) {
                        cpn = 0;
                        cdn = PN_INF;
                        cml = 0;
                    } else {
                        cpn = static_cast<uint32_t>(est);
                        cdn = 1;
                    }
                }
                undo_move(pos, m, u);

                if (cpn == 0) {
                    out_pn = 0;
                    out_dn = PN_INF;
                    out_best = m;
                    out_mate_len = static_cast<uint8_t>(cml + 1);
                    out_rep_dep = false;
                    if (!skip_root_tt) {
                        store_tt(pos.zobrist, rem, true, 0, PN_INF, m, out_mate_len);
                    }
                    return;
                }
                nm.pn[i] = cpn;
                nm.dn[i] = cdn;
                nm.mate_len[i] = cml;
                nm.rep_dep[i] = 0;
            }

            while (!check_abort()) {
                uint32_t min_pn = PN_INF;
                uint32_t second_pn = PN_INF;
                uint64_t sum_dn = 0;
                int best_i = -1;
                bool any_child_rd = any_rep_child;

                for (int i = 0; i < n; ++i) {
                    if (nm.pn[i] < min_pn) {
                        second_pn = min_pn;
                        min_pn = nm.pn[i];
                        best_i = i;
                    } else if (nm.pn[i] < second_pn) {
                        second_pn = nm.pn[i];
                    }
                    sum_dn += nm.dn[i];
                    if (nm.rep_dep[i]) any_child_rd = true;
                }

                uint32_t clamped_dn = static_cast<uint32_t>(std::min<uint64_t>(sum_dn, PN_INF));
                if (min_pn == 0) {
                    out_pn = 0;
                    out_dn = PN_INF;
                    out_best = nm.list.moves[best_i];
                    out_mate_len = static_cast<uint8_t>(nm.mate_len[best_i] + 1);
                    out_rep_dep = (nm.rep_dep[best_i] != 0);
                    if (out_rep_dep) dirty_rep_tt_ = true;
                    if (!skip_root_tt && !out_rep_dep) {
                        store_tt(pos.zobrist, rem, true, 0, PN_INF, out_best, out_mate_len);
                    }
                    return;
                }
                if (clamped_dn == 0 || min_pn >= th_pn || clamped_dn >= th_dn) {
                    out_pn = min_pn;
                    out_dn = clamped_dn;
                    out_best = (best_i >= 0) ? nm.list.moves[best_i] : Move::none();
                    out_rep_dep = any_child_rd;
                    if (out_rep_dep) dirty_rep_tt_ = true;
                    if (!skip_root_tt && !out_rep_dep) {
                        store_tt(pos.zobrist, rem, true, out_pn, out_dn, out_best, 0);
                    }
                    return;
                }

                const Move m = nm.list.moves[best_i];
                uint32_t margin = (second_pn >= PN_INF) ? 0 : std::max<uint32_t>(4, second_pn * 2);
                uint32_t cap_pn = (second_pn >= PN_INF || margin >= PN_INF - second_pn) ? PN_INF : (second_pn + margin + 1);
                uint32_t child_th_pn = std::min(th_pn, cap_pn);
                uint32_t other_dn = clamped_dn - nm.dn[best_i];
                uint32_t child_th_dn = (th_dn > other_dn) ? (th_dn - other_dn) : 1;

                UndoState u;
                apply_move(pos, m, u);
                path_hashes_[ply + 1] = pos.zobrist;

                uint32_t npn = 1, ndn = 1;
                Move dummy_bm = Move::none();
                uint8_t nml = 0;
                bool crd = false;
                dfpn(pos, rem - 1, ply + 1, false, child_th_pn, child_th_dn, npn, ndn, dummy_bm, nml, crd);
                undo_move(pos, m, u);
                if (abort_) {
                    out_pn = min_pn;
                    out_dn = clamped_dn;
                    return;
                }

                nm.pn[best_i] = npn;
                nm.dn[best_i] = ndn;
                nm.mate_len[best_i] = nml;
                nm.rep_dep[best_i] = crd ? 1 : 0;
            }
            out_pn = 1;
            out_dn = 1;
            return;
        } else {
            // AND node (defender)
            if (rem <= 0) {
                if (!has_any_legal_evasion(pos)) {
                    out_pn = 0;
                    out_dn = PN_INF;
                    out_mate_len = 0;
                    store_tt(pos.zobrist, rem, false, 0, PN_INF, Move::none(), 0);
                } else {
                    out_pn = PN_INF;
                    out_dn = 0;
                    store_tt(pos.zobrist, rem, false, PN_INF, 0, Move::none(), 0);
                }
                return;
            }

            DfpnNodeMoves& nm = (*stack_)[ply];
            generate_evasions(pos, nm.list);
            int n = nm.list.count;
            if (n == 0) {
                out_pn = 0;
                out_dn = PN_INF;
                out_mate_len = 0;
                store_tt(pos.zobrist, rem, false, 0, PN_INF, Move::none(), 0);
                return;
            }

            // Fast exact shortcut at rem == 2: test each evasion for mate-in-1 inline
            // (king/captures first), returning immediately on first surviving evasion.
            if (rem == 2) {
                bool any_rep = false;
                int non_rep_evasions = 0;
                uint8_t max_ml = 0;
                MoveList& chk_list = (*stack_)[ply + 1].list;
                for (int i = 0; i < n; ++i) {
                    if (check_abort()) {
                        out_pn = 1;
                        out_dn = 1;
                        return;
                    }
                    const Move m = nm.list.moves[i];
                    UndoState u;
                    apply_move(pos, m, u);
                    if (count_reps(pos.zobrist, ply + 1) >= 3) {
                        undo_move(pos, m, u);
                        any_rep = true;
                        if (max_ml < 1) max_ml = 1;
                        continue;
                    }
                    ++non_rep_evasions;
                    uint32_t cpn = 1, cdn = 1;
                    Move cbm = Move::none();
                    uint8_t cml = 0;
                    bool hit = lookup_tt(pos.zobrist, 1, true, cpn, cdn, cbm, cml);
                    if (hit && cpn == 0) {
                        undo_move(pos, m, u);
                        if (static_cast<uint8_t>(cml + 1) > max_ml) max_ml = static_cast<uint8_t>(cml + 1);
                        continue;
                    }
                    if (hit && cdn == 0) {
                        undo_move(pos, m, u);
                        out_pn = PN_INF;
                        out_dn = 0;
                        out_best = m;
                        out_rep_dep = false;
                        store_tt(pos.zobrist, rem, false, PN_INF, 0, m, 0);
                        return;
                    }

                    path_hashes_[ply + 1] = pos.zobrist;
                    ++nodes_;
                    generate_checks(pos, chk_list);
                    Move mate1 = Move::none();
                    bool any_rep_chk = false;
                    for (int j = 0; j < chk_list.count; ++j) {
                        const Move cm = chk_list.moves[j];
                        UndoState cu;
                        apply_move(pos, cm, cu);
                        ++nodes_;
                        if (count_reps(pos.zobrist, ply + 2) >= 3) {
                            undo_move(pos, cm, cu);
                            any_rep_chk = true;
                            continue;
                        }
                        bool has_ev = has_any_legal_evasion(pos);
                        undo_move(pos, cm, cu);
                        if (!has_ev) {
                            mate1 = cm;
                            break;
                        }
                    }
                    if (!mate1.is_none()) {
                        store_tt(pos.zobrist, 1, true, 0, PN_INF, mate1, 1);
                        undo_move(pos, m, u);
                        if (max_ml < 2) max_ml = 2;
                    } else {
                        if (!any_rep_chk) {
                            store_tt(pos.zobrist, 1, true, PN_INF, 0, Move::none(), 0);
                        }
                        undo_move(pos, m, u);
                        out_pn = PN_INF;
                        out_dn = 0;
                        out_best = m;
                        out_rep_dep = any_rep_chk;
                        if (out_rep_dep) dirty_rep_tt_ = true;
                        if (!any_rep_chk) {
                            store_tt(pos.zobrist, rem, false, PN_INF, 0, m, 0);
                        }
                        return;
                    }
                }
                // Điều 5 (Thiên Nhật Hòa): if EVERY legal evasion triggers rep3, defender gets a draw (NOT mated!)
                if (non_rep_evasions == 0) {
                    out_pn = PN_INF;
                    out_dn = 0;
                    out_best = nm.list.moves[0];
                    out_rep_dep = true;
                    dirty_rep_tt_ = true;
                    return;
                }
                out_pn = 0;
                out_dn = PN_INF;
                out_mate_len = max_ml;
                out_rep_dep = any_rep;
                if (out_rep_dep) dirty_rep_tt_ = true;
                if (!any_rep) {
                    store_tt(pos.zobrist, rem, false, 0, PN_INF, Move::none(), max_ml);
                }
                return;
            }

            bool any_rep_child = false;
            int non_rep_evasions = 0;
            int own_king = find_king_sq(pos, static_cast<Side>(pos.side_to_move));
            for (int i = 0; i < n; ++i) {
                const Move m = nm.list.moves[i];
                UndoState u;
                apply_move(pos, m, u);
                if (count_reps(pos.zobrist, ply + 1) >= 3) {
                    undo_move(pos, m, u);
                    nm.pn[i] = 0;
                    nm.dn[i] = PN_INF;
                    nm.mate_len[i] = 0;
                    nm.rep_dep[i] = 1;
                    any_rep_child = true;
                    continue;
                }
                ++non_rep_evasions;
                uint32_t cpn = 1, cdn = 1;
                Move cbm = Move::none();
                uint8_t cml = 0;
                if (!lookup_tt(pos.zobrist, rem - 1, true, cpn, cdn, cbm, cml)) {
                    cpn = 1;
                    // Calibrated evasion effort: settle king escapes & captures deeply
                    // before expanding futile muda-ai drop interpositions.
                    if (m.kind != MoveKind::Drop && m.src == own_king) cdn = 1;
                    else if (m.kind != MoveKind::Drop) cdn = 4;
                    else cdn = 128;
                }
                undo_move(pos, m, u);

                if (cdn == 0) {
                    out_pn = PN_INF;
                    out_dn = 0;
                    out_best = m;
                    out_rep_dep = false;
                    store_tt(pos.zobrist, rem, false, PN_INF, 0, m, 0);
                    return;
                }
                nm.pn[i] = cpn;
                nm.dn[i] = cdn;
                nm.mate_len[i] = cml;
                nm.rep_dep[i] = 0;
            }

            // Điều 5 (Thiên Nhật Hòa): every legal evasion triggers rep3 -> forced draw, NOT mate!
            if (non_rep_evasions == 0) {
                out_pn = PN_INF;
                out_dn = 0;
                out_best = nm.list.moves[0];
                out_rep_dep = true;
                dirty_rep_tt_ = true;
                return;
            }

            while (!check_abort()) {
                uint64_t sum_pn = 0;
                uint32_t min_dn = PN_INF;
                uint32_t second_dn = PN_INF;
                int best_i = -1;
                uint8_t max_ml = 0;
                bool any_child_rd = any_rep_child;

                for (int i = 0; i < n; ++i) {
                    if (nm.dn[i] < min_dn) {
                        second_dn = min_dn;
                        min_dn = nm.dn[i];
                        best_i = i;
                    } else if (nm.dn[i] < second_dn) {
                        second_dn = nm.dn[i];
                    }
                    sum_pn += nm.pn[i];
                    if (nm.pn[i] == 0 && static_cast<uint8_t>(nm.mate_len[i] + 1) > max_ml) {
                        max_ml = static_cast<uint8_t>(nm.mate_len[i] + 1);
                    }
                    if (nm.rep_dep[i]) any_child_rd = true;
                }

                uint32_t clamped_pn = static_cast<uint32_t>(std::min<uint64_t>(sum_pn, PN_INF));
                if (min_dn == 0) {
                    out_pn = PN_INF;
                    out_dn = 0;
                    out_best = nm.list.moves[best_i];
                    out_rep_dep = (nm.rep_dep[best_i] != 0);
                    if (out_rep_dep) dirty_rep_tt_ = true;
                    if (!out_rep_dep) {
                        store_tt(pos.zobrist, rem, false, PN_INF, 0, out_best, 0);
                    }
                    return;
                }
                if (clamped_pn == 0) {
                    out_pn = 0;
                    out_dn = PN_INF;
                    out_mate_len = max_ml;
                    out_rep_dep = any_child_rd;
                    if (out_rep_dep) dirty_rep_tt_ = true;
                    if (!out_rep_dep) {
                        store_tt(pos.zobrist, rem, false, 0, PN_INF, Move::none(), max_ml);
                    }
                    return;
                }
                if (clamped_pn >= th_pn || min_dn >= th_dn) {
                    out_pn = clamped_pn;
                    out_dn = min_dn;
                    out_best = (best_i >= 0) ? nm.list.moves[best_i] : Move::none();
                    out_rep_dep = any_child_rd;
                    if (out_rep_dep) dirty_rep_tt_ = true;
                    if (!out_rep_dep) {
                        store_tt(pos.zobrist, rem, false, out_pn, out_dn, out_best, 0);
                    }
                    return;
                }

                const Move m = nm.list.moves[best_i];
                uint32_t margin = (second_dn >= PN_INF) ? 0 : std::max<uint32_t>(4, second_dn * 2);
                uint32_t cap_dn = (second_dn >= PN_INF || margin >= PN_INF - second_dn) ? PN_INF : (second_dn + margin + 1);
                uint32_t child_th_dn = std::min(th_dn, cap_dn);
                uint32_t other_pn = clamped_pn - nm.pn[best_i];
                uint32_t child_th_pn = (th_pn > other_pn) ? (th_pn - other_pn) : 1;

                UndoState u;
                apply_move(pos, m, u);
                path_hashes_[ply + 1] = pos.zobrist;

                uint32_t npn = 1, ndn = 1;
                Move dummy_bm = Move::none();
                uint8_t nml = 0;
                bool crd = false;
                dfpn(pos, rem - 1, ply + 1, true, child_th_pn, child_th_dn, npn, ndn, dummy_bm, nml, crd);
                undo_move(pos, m, u);
                if (abort_) {
                    out_pn = clamped_pn;
                    out_dn = min_dn;
                    return;
                }

                nm.pn[best_i] = npn;
                nm.dn[best_i] = ndn;
                nm.mate_len[best_i] = nml;
                nm.rep_dep[best_i] = crd ? 1 : 0;
            }
            out_pn = 1;
            out_dn = 1;
            return;
        }
    }

    bool extract_pv(
        const Position& root_pos,
        int rem,
        std::vector<Move>& out_pv,
        Move hint_best,
        bool& out_rep_dep,
        bool extra_grace = false
    ) {
        out_pv.clear();
        out_rep_dep = false;
        Position sim = root_pos;
        path_hashes_[0] = sim.zobrist;
        int cur_rem = rem;
        int ply = 0;

        const int64_t saved_start_ms = start_ms_;
        const int64_t saved_budget_ms = time_budget_ms_;
        const uint64_t saved_max_nodes = max_nodes_;

        abort_ = false;
        int64_t grace_ms = 0;
        uint64_t grace_nodes = 0;
        if (saved_max_nodes <= 10000ULL) {
            // Bounded internal pvlight call inside alpha-beta: keep PV extraction strictly lightweight!
            grace_ms = 25;
            grace_nodes = std::min<uint64_t>(2500ULL, saved_max_nodes * 4ULL);
        } else if (saved_budget_ms > 0) {
            int64_t spent = now_ms() - saved_start_ms;
            int64_t rem_ms = saved_budget_ms - spent;
            int64_t cap_extra = std::min<int64_t>(saved_budget_ms / 4, extra_grace ? 180 : 120);
            grace_ms = std::max<int64_t>(std::max<int64_t>(rem_ms, cap_extra), 20);
            grace_nodes = std::min<uint64_t>(extra_grace ? 600000ULL : 400000ULL, saved_max_nodes);
        } else {
            grace_ms = extra_grace ? 800 : 300;
            grace_nodes = extra_grace ? 900000ULL : 600000ULL;
        }
        start_ms_ = now_ms();
        time_budget_ms_ = grace_ms;
        max_nodes_ = nodes_ + grace_nodes;

        bool reached_terminal = false;
        MoveList evasions;
        std::vector<int> ev_len;
        std::vector<bool> ev_rep3;
        std::vector<bool> ev_exact;
        std::vector<int> ev_disproved;
        std::vector<Move> ev_best_move;
        Move next_or_hint = Move::none();
        int parent_second_worst = 0;

        while (cur_rem > 0 && ply < MAX_MATE_DEPTH && !check_abort()) {
            if ((ply & 1) == 0) {
                Move m = Move::none();
                uint32_t tpn = 1, tdn = 1;
                uint8_t tml = 0;
                bool dummy_rd = false;
                if (ply == 0 && !hint_best.is_none() && !is_root_excluded(hint_best)) {
                    m = hint_best;
                    next_or_hint = Move::none();
                } else if (!next_or_hint.is_none()) {
                    m = next_or_hint;
                    next_or_hint = Move::none();
                } else {
                    if (!lookup_tt(sim.zobrist, cur_rem, true, tpn, tdn, m, tml) || tpn != 0 || m.is_none()) {
                        tpn = 1; tdn = 1; m = Move::none(); tml = 0;
                        dfpn(sim, cur_rem, ply, true, PN_INF, PN_INF, tpn, tdn, m, tml, dummy_rd);
                        if (dummy_rd) out_rep_dep = true;
                        if (tpn != 0) m = Move::none();
                    }
                }
                if (m.is_none()) break;

                if (!extra_grace && ply >= 2 && cur_rem >= 3 && cur_rem <= 9 &&
                    nodes_ + 20000ULL < max_nodes_ &&
                    !(stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) &&
                    (now_ms() - start_ms_) < (grace_ms / 2)) {
                    int min_wlim = std::max(1, parent_second_worst);
                    if ((min_wlim & 1) == 0) ++min_wlim;
                    const int first_w = std::max(min_wlim, (cur_rem >= 7) ? (cur_rem - 4) : (cur_rem - 2));
                    uint64_t saved_cap = max_nodes_;
                    for (int wlim = first_w; wlim <= cur_rem - 2; wlim += 2) {
                        if (wlim < 1) continue;
                        uint64_t wcap = (wlim <= 5) ? 8000ULL : 15000ULL;
                        max_nodes_ = std::min(saved_cap, nodes_ + wcap);
                        abort_ = false;
                        uint32_t wpn = 1, wdn = 1;
                        Move wm = Move::none();
                        uint8_t wml = 0;
                        bool wrd = false;
                        dfpn(sim, wlim, ply, true, PN_INF, PN_INF, wpn, wdn, wm, wml, wrd);
                        max_nodes_ = saved_cap;
                        abort_ = false;
                        if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) abort_ = true;
                        if (time_budget_ms_ > 0 && (now_ms() - start_ms_) >= time_budget_ms_) abort_ = true;
                        if (!abort_ && wpn == 0 && !wm.is_none()) {
                            int eff_wml = (wml > 0) ? static_cast<int>(wml) : wlim;
                            // Never shorten an attacker branch below a sibling evasion's proved defense length,
                            // which would falsely shrink the PV length below the defender's true resistance.
                            if (eff_wml >= parent_second_worst) {
                                m = wm;
                                if (wrd) out_rep_dep = true;
                                if (eff_wml < cur_rem) {
                                    cur_rem = eff_wml;
                                }
                            }
                            break;
                        }
                        if (abort_) break;
                    }
                    if (abort_) break;
                }

                out_pv.push_back(m);
                UndoState u;
                apply_move(sim, m, u);
                ++ply;
                --cur_rem;
                path_hashes_[ply] = sim.zobrist;
                if (count_reps(sim.zobrist, ply) >= 3) {
                    // Attacker triggered 3-fold repetition -> illegal winning path!
                    out_pv.pop_back();
                    break;
                }
                if (!has_any_legal_evasion(sim)) {
                    reached_terminal = true;
                    break;
                }
            } else {
                generate_evasions(sim, evasions);
                if (evasions.count == 0) {
                    reached_terminal = true;
                    break;
                }

                ev_len.assign(evasions.count, -1);
                ev_rep3.assign(evasions.count, false);
                ev_exact.assign(evasions.count, false);
                ev_disproved.assign(evasions.count, 0);
                ev_best_move.assign(evasions.count, Move::none());
                int worst_idx = -1;
                int worst_len = -1;
                bool worst_is_rep3 = false;
                bool all_evasions_proved = true;

                for (int i = 0; i < evasions.count; ++i) {
                    if (check_abort()) {
                        all_evasions_proved = false;
                        break;
                    }
                    const Move em = evasions.moves[i];
                    UndoState u;
                    apply_move(sim, em, u);
                    if (count_reps(sim.zobrist, ply + 1) >= 3) {
                        undo_move(sim, em, u);
                        ev_len[i] = 0;
                        ev_rep3[i] = true;
                        ev_exact[i] = true;
                        if (worst_len < 0) {
                            worst_len = 0;
                            worst_idx = i;
                            worst_is_rep3 = true;
                        }
                        continue;
                    }

                    uint32_t cpn = 1, cdn = 1;
                    Move cm = Move::none();
                    uint8_t cml = 0;
                    bool crd = false;
                    path_hashes_[ply + 1] = sim.zobrist;

                    // Shallow blunder filter near root (1, 3, 5 ply) so short-blunder evasions
                    // are never picked over stubborn longest-defense evasions.
                    int shallow_lim = std::min(5, cur_rem - 1);
                    bool solved_short = false;
                    int local_disproved = 0;
                    bool had_prior_shallow_abort = false;
                    if (shallow_lim >= 1) {
                        if (lookup_tt(sim.zobrist, shallow_lim, true, cpn, cdn, cm, cml) && cpn == 0) {
                            solved_short = true;
                            if (static_cast<int>(cml) <= 1) ev_exact[i] = true;
                        } else if (cur_rem - 1 > 5 && ply <= 5) {
                            uint64_t saved_cap = max_nodes_;
                            max_nodes_ = std::min(saved_cap, nodes_ + 1200ULL);
                            for (int slim = 1; slim <= 5; slim += 2) {
                                cpn = 1; cdn = 1; cm = Move::none(); cml = 0;
                                dfpn(sim, slim, ply + 1, true, PN_INF, PN_INF, cpn, cdn, cm, cml, crd);
                                if (cpn == 0) {
                                    solved_short = true;
                                    if (!abort_ && !had_prior_shallow_abort && static_cast<int>(cml) <= slim) {
                                        ev_exact[i] = true;
                                    }
                                    break;
                                }
                                if (abort_) {
                                    had_prior_shallow_abort = true;
                                    break;
                                }
                                if (cdn == 0) {
                                    local_disproved = std::max(local_disproved, slim);
                                }
                            }
                            max_nodes_ = saved_cap;
                            abort_ = false;
                            if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) abort_ = true;
                            if (time_budget_ms_ > 0 && (now_ms() - start_ms_) >= time_budget_ms_) abort_ = true;
                        }
                    }

                    if (!solved_short) {
                        cpn = 1; cdn = 1; cm = Move::none(); cml = 0;
                        if (!lookup_tt(sim.zobrist, cur_rem - 1, true, cpn, cdn, cm, cml) || cpn != 0) {
                            dfpn(sim, cur_rem - 1, ply + 1, true, PN_INF, PN_INF, cpn, cdn, cm, cml, crd);
                        }
                    }
                    undo_move(sim, em, u);

                    if (cpn == 0) {
                        int mlen = static_cast<int>(cml);
                        ev_len[i] = mlen;
                        ev_rep3[i] = false;
                        ev_disproved[i] = local_disproved;
                        ev_best_move[i] = cm;
                        if (mlen <= 1 || mlen <= local_disproved + 2) {
                            ev_exact[i] = true;
                        }
                        if (mlen > worst_len) {
                            worst_len = mlen;
                            worst_idx = i;
                            worst_is_rep3 = false;
                        }
                    } else {
                        all_evasions_proved = false;
                        break;
                    }
                }
                if (abort_ || !all_evasions_proved || worst_idx < 0) break;
                // Điều 5 (Thiên Nhật Hòa): if EVERY legal evasion is rep3, defender gets a draw (NOT mate)!
                if (worst_is_rep3) {
                    out_rep_dep = true;
                    break;
                }

                // Longest-defense refinement at ply 1, 3, 5: ensure the selected defense move `worst_idx`
                // does not actually fall into a strictly shorter mate than an alternative evasion.
                if (!extra_grace && (ply == 1 || ply == 3 || ply == 5) && !worst_is_rep3 && worst_len >= 5 && evasions.count >= 2) {
                    const int64_t ref_ms_cap = (ply == 1) ? 220 : ((ply == 3) ? 120 : 60);
                    const uint64_t ref_nodes_cap = (ply == 1) ? 180000ULL : ((ply == 3) ? 80000ULL : 40000ULL);
                    const int64_t ref_deadline_ms = now_ms() + std::min<int64_t>(ref_ms_cap, grace_ms / 3);
                    const uint64_t ref_node_limit = nodes_ + std::min<uint64_t>(
                        ref_nodes_cap, (max_nodes_ > nodes_ ? (max_nodes_ - nodes_) / 3 : 0ULL));

                    for (int ref_round = 0; ref_round < 8 && !check_abort(); ++ref_round) {
                        if (ev_exact[worst_idx] || ev_len[worst_idx] < 5) break;
                        if (now_ms() >= ref_deadline_ms || nodes_ >= ref_node_limit) break;

                        int second_worst_len = -1;
                        for (int i = 0; i < evasions.count; ++i) {
                            if (i != worst_idx && !ev_rep3[i] && ev_len[i] > second_worst_len) {
                                second_worst_len = ev_len[i];
                            }
                        }
                        if (second_worst_len < 3) break;

                        const Move em = evasions.moves[worst_idx];
                        UndoState u;
                        apply_move(sim, em, u);
                        path_hashes_[ply + 1] = sim.zobrist;

                        int start_lim = (ev_disproved[worst_idx] == 0) ? 3 : (ev_disproved[worst_idx] + 2);
                        if ((start_lim & 1) == 0) ++start_lim;
                        if (start_lim < 3) start_lim = 3;
                        const int max_test_lim = std::min(ev_len[worst_idx] - 2, second_worst_len);

                        bool improved_short = false;
                        const uint64_t saved_cap = max_nodes_;
                        for (int test_lim = start_lim; test_lim <= max_test_lim && !(stop_flag_ && stop_flag_->load(std::memory_order_relaxed)); test_lim += 2) {
                            if (now_ms() >= ref_deadline_ms || nodes_ >= ref_node_limit ||
                                (now_ms() - start_ms_) >= grace_ms || saved_cap <= nodes_) {
                                break;
                            }
                            uint64_t lim_cap = (test_lim <= 5) ? 30000ULL : ((test_lim <= 9) ? 80000ULL : 100000ULL);
                            uint64_t cap_ceiling = std::min(saved_cap, ref_node_limit);
                            if (cap_ceiling <= nodes_) break;
                            max_nodes_ = std::min(cap_ceiling, nodes_ + lim_cap);
                            time_budget_ms_ = std::min<int64_t>(grace_ms, std::max<int64_t>(1, ref_deadline_ms - start_ms_));
                            abort_ = false;

                            uint32_t cpn = 1, cdn = 1;
                            Move cm = Move::none();
                            uint8_t cml = 0;
                            bool crd = false;
                            dfpn(sim, test_lim, ply + 1, true, PN_INF, PN_INF, cpn, cdn, cm, cml, crd);

                            bool timed_out_or_stopped =
                                (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) ||
                                (now_ms() >= ref_deadline_ms) ||
                                ((now_ms() - start_ms_) >= grace_ms);
                            bool step_aborted = abort_ || timed_out_or_stopped;

                            if (!step_aborted && cpn == 0 && static_cast<int>(cml) < ev_len[worst_idx]) {
                                ev_len[worst_idx] = static_cast<int>(cml);
                                ev_best_move[worst_idx] = cm;
                                if (ev_len[worst_idx] <= ev_disproved[worst_idx] + 2) {
                                    ev_exact[worst_idx] = true;
                                }
                                improved_short = true;
                                break;
                            } else if (!step_aborted && cdn == 0) {
                                ev_disproved[worst_idx] = std::max(ev_disproved[worst_idx], test_lim);
                                if (ev_disproved[worst_idx] >= ev_len[worst_idx] - 2) {
                                    ev_exact[worst_idx] = true;
                                    break;
                                }
                            } else {
                                ev_exact[worst_idx] = true;
                                break;
                            }
                        }
                        max_nodes_ = saved_cap;
                        time_budget_ms_ = grace_ms;
                        undo_move(sim, em, u);
                        abort_ = false;
                        if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) abort_ = true;
                        if (time_budget_ms_ > 0 && (now_ms() - start_ms_) >= grace_ms) abort_ = true;

                        if (improved_short) {
                            worst_len = -1;
                            worst_is_rep3 = false;
                            for (int i = 0; i < evasions.count; ++i) {
                                if (!ev_rep3[i] && ev_len[i] > worst_len) {
                                    worst_len = ev_len[i];
                                    worst_idx = i;
                                    worst_is_rep3 = false;
                                }
                            }
                            if (worst_len < 0) break;
                        } else {
                            break;
                        }
                    }
                }

                int final_second_worst = 0;
                for (int i = 0; i < evasions.count; ++i) {
                    if (i != worst_idx && !ev_rep3[i] && ev_len[i] > final_second_worst) {
                        final_second_worst = ev_len[i];
                    }
                }
                parent_second_worst = final_second_worst;

                if (!worst_is_rep3 && worst_len > 0 && worst_len < cur_rem - 1) {
                    cur_rem = worst_len + 1;
                }
                next_or_hint = (!worst_is_rep3) ? ev_best_move[worst_idx] : Move::none();

                const Move worst_m = evasions.moves[worst_idx];
                out_pv.push_back(worst_m);
                UndoState u;
                apply_move(sim, worst_m, u);
                ++ply;
                --cur_rem;
                path_hashes_[ply] = sim.zobrist;
                if (ev_rep3[worst_idx] || count_reps(sim.zobrist, ply) >= 3) {
                    out_rep_dep = true;
                    reached_terminal = true;
                    break;
                }
            }
        }

        if (out_pv.empty() && !hint_best.is_none()) {
            out_pv.push_back(hint_best);
        }

        start_ms_ = saved_start_ms;
        time_budget_ms_ = saved_budget_ms;
        max_nodes_ = saved_max_nodes;
        abort_ = false;
        if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) abort_ = true;
        if (time_budget_ms_ > 0 && (now_ms() - start_ms_) >= time_budget_ms_) abort_ = true;

        return reached_terminal;
    }
};

} // namespace

void clear_dfpn_tt() {
    get_dfpn_storage().invalidate();
}

MateResult solve_mate(
    const Position& root_pos,
    int max_ply,
    int64_t time_budget_ms,
    uint64_t max_nodes,
    const std::atomic<bool>* stop_flag,
    const std::vector<RepHashEntry>* base_rep,
    bool find_shortest,
    bool reuse_tt,
    const Move* excluded_moves,
    int num_excluded,
    std::atomic<uint64_t>* live_nodes_counter
) {
    ensure_mate_init();
    DfpnSolver solver(
        time_budget_ms,
        max_nodes,
        stop_flag,
        base_rep,
        nullptr,
        0,
        reuse_tt,
        excluded_moves,
        num_excluded,
        live_nodes_counter
    );
    return solver.solve(root_pos, max_ply, find_shortest);
}

bool solve_mate(const Position& root_pos, int max_ply, Move& out_best_move, int& out_mate_ply) {
    MateResult res = solve_mate(root_pos, max_ply);
    if (res.found && !res.pv.empty()) {
        out_best_move = res.pv.front();
        out_mate_ply = res.mate_ply;
        return true;
    }
    return false;
}

bool has_mate_in_1(
    Position& pos,
    Move& out_move,
    const uint64_t* path_hashes,
    int ply,
    const std::vector<RepHashEntry>* base_rep,
    int8_t known_in_check
) {
    ensure_mate_init();
    static thread_local MoveList tl_checks;
    generate_checks_impl<false>(pos, tl_checks, known_in_check);
    for (int i = 0; i < tl_checks.count; ++i) {
        const Move& m = tl_checks.moves[i];
        UndoState u;
        apply_move(pos, m, u);
        int cnt = 1;
        if (base_rep) {
            for (const auto& e : *base_rep) {
                if (e.hash == pos.zobrist) {
                    cnt += e.count;
                    break;
                }
            }
        }
        if (path_hashes) {
            int start_p = base_rep ? 1 : 0;
            for (int k = start_p; k <= ply; ++k) {
                if (path_hashes[k] == pos.zobrist && path_hashes[k] != 0ULL) ++cnt;
            }
        }
        if (cnt >= 3) {
            undo_move(pos, m, u);
            continue;
        }
        bool has_ev = has_any_legal_evasion(pos);
        undo_move(pos, m, u);
        if (!has_ev) {
            out_move = m;
            return true;
        }
    }
    return false;
}

MateResult solve_mate_bounded(
    const Position& root_pos,
    int max_ply,
    uint64_t max_nodes,
    const std::atomic<bool>* stop_flag,
    const std::vector<RepHashEntry>* base_rep,
    const uint64_t* prior_path,
    int prior_ply
) {
    ensure_mate_init();
    DfpnSolver solver(80, max_nodes, stop_flag, base_rep, prior_path, prior_ply, true);
    return solver.solve(root_pos, max_ply, false);
}

} // namespace homei

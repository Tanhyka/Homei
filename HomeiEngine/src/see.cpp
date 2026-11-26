#include "see.h"
#include "attack.h"
#include "bitboard.h"
#include "eval.h"
#include "geometry.h"
#include <algorithm>
#include <chrono>

namespace homei {

namespace {

// Order of piece types from least valuable to most valuable for SEE least-valuable-attacker selection:
// BINH (100) < XA (280) < SI (320) < KI (360) < TUONG (420) < PHUONG (520) < LONG (600) < VUONG (10000)
constexpr uint8_t SEE_ORDER[8] = {BINH, XA, SI, KI, TUONG, PHUONG, LONG, VUONG};

struct StackOverride {
    int sq;
    uint8_t next_idx; // next buried index below `cur`, or BURIED_NONE
    Piece cur;
};

inline Piece get_active_piece(const Position& pos, int sq, const StackOverride* ovs, int n_ov) {
    for (int i = 0; i < n_ov; ++i) {
        if (ovs[i].sq == sq) return ovs[i].cur;
    }
    return pos.top_piece[sq];
}

inline int find_override(int sq, const StackOverride* ovs, int n_ov) {
    for (int i = 0; i < n_ov; ++i) {
        if (ovs[i].sq == sq) return i;
    }
    return -1;
}

// Computes newly uncovered slider X-ray attackers on `target_sq` under `occ` (respecting any active stack overrides).
inline Bitboard xray_sliders_to(const Position& pos, int target_sq, const Bitboard& occ, const StackOverride* ovs, int n_ov) {
    Bitboard res;
    const Bitboard ortho = ortho_attacks(target_sq, occ) & occ;
    const Bitboard diag = diag_attacks(target_sq, occ) & occ;

    // Non-overridden sliders via bitboards
    Bitboard ov_mask;
    for (int i = 0; i < n_ov; ++i) ov_mask.set_bit(ovs[i].sq);

    const Bitboard base_occ = occ & ~ov_mask;
    res |= (ortho & base_occ & pos.bb_type[LONG]);
    res |= (diag  & base_occ & pos.bb_type[PHUONG]);

    Bitboard xa_hits = ortho & base_occ & pos.bb_type[XA];
    while (xa_hits.any()) {
        int xsq = xa_hits.pop_lsb();
        if (BB_RAY_ORTHO[xsq][pos.top_piece[xsq].facing].test_bit(target_sq)) {
            res.set_bit(xsq);
        }
    }

    // Overridden squares (exposed buried pieces on a stack that stayed in `occ`)
    Bitboard ov_hits = (ortho | diag) & ov_mask;
    while (ov_hits.any()) {
        int osq = ov_hits.pop_lsb();
        Piece p = get_active_piece(pos, osq, ovs, n_ov);
        if (p.type == LONG && ortho.test_bit(osq)) res.set_bit(osq);
        else if (p.type == PHUONG && diag.test_bit(osq)) res.set_bit(osq);
        else if (p.type == XA && ortho.test_bit(osq) && BB_RAY_ORTHO[osq][p.facing].test_bit(target_sq)) res.set_bit(osq);
    }

    return res;
}

// Pure bitboard Stack-SEE implementation (zero apply_move/undo_move, zero Position copy).
int stack_see_core(const Position& pos, const Move& m, bool has_threshold, int threshold) {
    if (!is_capture_move(pos, m)) return 0;

    const int dst = m.dst;
    const int src = m.src;
    const Side orig_stm = static_cast<Side>(pos.side_to_move);

    const int v_dst = cell_exchange_swing(pos, dst);
    const int v_first = (m.kind == MoveKind::Team)
        ? cell_exchange_swing(pos, src)
        : piece_exchange_swing(pos.top_piece[src].type);

    // Quick threshold check before computing attackers if we have a threshold
    if (has_threshold) {
        if (v_dst < threshold) return v_dst; // even free capture doesn't reach threshold
        if (v_dst - v_first >= threshold) return v_dst - v_first; // even losing attacker still meets threshold
    }

    constexpr int MAX_SEE_DEPTH = 12;
    int gain[MAX_SEE_DEPTH + 1]{};
    gain[0] = v_dst;

    Bitboard occ = pos.bb_occ;
    StackOverride ovs[MAX_SEE_DEPTH + 2];
    int n_ov = 0;

    // Apply initial move on bitboard occupancy & override list
    bool src_has_buried = (m.kind == MoveKind::Single && pos.height[src] >= 2);
    if (src_has_buried) {
        uint8_t idx = pos.buried_head[src];
        ovs[n_ov++] = {src, pos.buried_next[idx], pos.buried_piece[idx]};
    } else {
        occ.clear_bit(src);
    }

    // Compute initial attackers on `dst` under updated `occ` BEFORE expensive slider_blockers!
    Bitboard attackers = attackers_to_both_bb(pos, dst, occ);
    if (src_has_buried) {
        attackers.clear_bit(src);
        const Piece& bp = ovs[0].cur;
        if (piece_attacks_bb_occ(src, bp.type, bp.facing, occ).test_bit(dst)) {
            attackers.set_bit(src);
        }
    }
    attackers.clear_bit(dst);

    Side turn = other_side(orig_stm);
    // Ultra-fast early exit: undefended capture! Skips BOTH slider_blockers calls and entire loop!
    if ((attackers & pos.bb_side[turn] & occ).empty()) {
        return v_dst;
    }

    // Compute pin blockers LAZILY per side (only when that side actually recaptures!)
    Bitboard pinners[2];
    Bitboard blockers[2];
    bool pins_ready[2] = {false, false};

    int v_on_sq = v_first;
    int d = 1;

    while (d < MAX_SEE_DEPTH) {
        Bitboard stm_att = attackers & pos.bb_side[turn] & occ;
        if (stm_att.empty()) break;

        if (!pins_ready[turn]) {
            blockers[turn] = slider_blockers(pos, turn, pinners[turn]);
            pins_ready[turn] = true;
        }

        // Pick least-valuable legal (non-pinned-off-ray) attacker of `turn`
        int chosen_sq = -1;
        uint8_t chosen_pt = 0;
        int chosen_swing = 1000000;
        const int ksq = pos.king_sq[turn];
        const bool check_pin = (ksq < NUM_SQUARES && (stm_att & blockers[turn]).any() && (pinners[turn] & occ).any());

        if (n_ov == 0 && !check_pin) {
            // Ultra-fast O(1) bitboard path (95%+ of SEE plies!)
            for (int oi = 0; oi < 8; ++oi) {
                uint8_t pt = SEE_ORDER[oi];
                Bitboard cand = stm_att & pos.bb_type[pt];
                if (cand.any()) {
                    chosen_sq = cand.lsb();
                    chosen_pt = pt;
                    chosen_swing = piece_exchange_swing(pt);
                    break;
                }
            }
        } else {
            // General path handling overrides and pinned pieces
            Bitboard tmp = stm_att;
            while (tmp.any()) {
                int asq = tmp.pop_lsb();
                int oi = find_override(asq, ovs, n_ov);
                bool will_empty = (oi >= 0) ? (ovs[oi].next_idx == BURIED_NONE) : (pos.height[asq] <= 1);
                if (check_pin && will_empty && blockers[turn].test_bit(asq)) {
                    if (!BB_LINE[ksq][asq].test_bit(dst)) {
                        // Only pinned if a pinner on the same ray is still on the board
                        if ((BB_LINE[ksq][asq] & pinners[turn] & occ).any()) {
                            continue;
                        }
                    }
                }
                Piece p = (oi >= 0) ? ovs[oi].cur : pos.top_piece[asq];
                int sw = piece_exchange_swing(p.type);
                if (sw < chosen_swing) {
                    chosen_swing = sw;
                    chosen_sq = asq;
                    chosen_pt = p.type;
                }
            }
        }

        if (chosen_sq < 0) break;

        // King cannot recapture if opponent still has at least one attacker left on `dst`
        if (chosen_pt == VUONG) {
            Bitboard opp_att = attackers & pos.bb_side[other_side(turn)] & occ;
            if (opp_att.any()) break;
        }

        gain[d] = v_on_sq - gain[d - 1];
        v_on_sq = chosen_swing;

        // Advance `chosen_sq`: either uncover buried piece in stack or vacate square and uncover X-rays
        int oi = find_override(chosen_sq, ovs, n_ov);
        if (oi >= 0) {
            if (ovs[oi].next_idx != BURIED_NONE) {
                uint8_t nxt = ovs[oi].next_idx;
                ovs[oi].cur = pos.buried_piece[nxt];
                ovs[oi].next_idx = pos.buried_next[nxt];
                if (!piece_attacks_bb_occ(chosen_sq, ovs[oi].cur.type, ovs[oi].cur.facing, occ).test_bit(dst)) {
                    attackers.clear_bit(chosen_sq);
                }
            } else {
                occ.clear_bit(chosen_sq);
                attackers.clear_bit(chosen_sq);
                attackers |= xray_sliders_to(pos, dst, occ, ovs, n_ov);
            }
        } else if (pos.height[chosen_sq] >= 2) {
            uint8_t idx = pos.buried_head[chosen_sq];
            Piece bp = pos.buried_piece[idx];
            ovs[n_ov++] = {chosen_sq, pos.buried_next[idx], bp};
            if (!piece_attacks_bb_occ(chosen_sq, bp.type, bp.facing, occ).test_bit(dst)) {
                attackers.clear_bit(chosen_sq);
            }
        } else {
            occ.clear_bit(chosen_sq);
            attackers.clear_bit(chosen_sq);
            attackers |= xray_sliders_to(pos, dst, occ, ovs, n_ov);
        }

        turn = other_side(turn);
        ++d;
    }

    while (--d) {
        gain[d - 1] = -std::max(-gain[d - 1], gain[d]);
    }
    return gain[0];
}

int square_hanging_loss_impl(const Position& pos, int sq, bool has_att_bb, const Bitboard* bb_att) {
    if (pos.height[sq] == 0 || pos.cell_contains_king(sq)) return 0;

    Side owner = pos.cell_owner(sq);
    Side attacker_side = other_side(owner);

    if (has_att_bb) {
        if (!bb_att[attacker_side].test_bit(sq)) return 0;
        if (!bb_att[owner].test_bit(sq)) {
            return cell_material_value(pos, sq);
        }
    }

    // Single pass fetches attackers of BOTH sides!
    Bitboard all_att = attackers_to_both_bb(pos, sq, pos.bb_occ);
    Bitboard att_bb = all_att & pos.bb_side[attacker_side];
    if (att_bb.empty()) return 0;

    int v_target = cell_material_value(pos, sq);
    Bitboard def_bb = all_att & pos.bb_side[owner];
    if (def_bb.empty()) {
        return v_target;
    }

    int swing_target = cell_exchange_swing(pos, sq);

    int total_attack_units = 0;
    int min_attacker_swing = 100000;
    while (att_bb.any()) {
        int asq = att_bb.pop_lsb();
        ++total_attack_units;
        int aswing = piece_exchange_swing(pos.top_piece[asq].type);
        if (aswing < min_attacker_swing) min_attacker_swing = aswing;
        if (pos.height[asq] >= 2) {
            Piece sec = pos.cell_second(asq);
            if (piece_attacks_bb_occ(asq, sec.type, sec.facing, pos.bb_occ).test_bit(sq)) {
                ++total_attack_units;
            }
        }
    }

    int total_defend_units = 0;
    int min_defender_swing = 100000;
    while (def_bb.any()) {
        int dsq = def_bb.pop_lsb();
        ++total_defend_units;
        int dswing = piece_exchange_swing(pos.top_piece[dsq].type);
        if (dswing < min_defender_swing) min_defender_swing = dswing;
        if (pos.height[dsq] >= 2) {
            Piece sec = pos.cell_second(dsq);
            if (piece_attacks_bb_occ(dsq, sec.type, sec.facing, pos.bb_occ).test_bit(sq)) {
                ++total_defend_units;
            }
        }
    }

    if (swing_target > min_attacker_swing) {
        return (swing_target - min_attacker_swing) / 2;
    }
    if (total_attack_units > total_defend_units) {
        if (swing_target + min_defender_swing > min_attacker_swing) {
            return (v_target * 2) / 3;
        }
    }
    return 0;
}

} // namespace

int stack_see_estimate(const Position& pos, const Move& m) {
#ifdef HOMEI_PROFILE
    SearchProfile& prof = get_search_profile();
    if (prof.enabled) {
        ++prof.see_calls;
        auto t0 = std::chrono::steady_clock::now();
        int r = stack_see_core(pos, m, false, 0);
        prof.see_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        return r;
    }
#endif
    return stack_see_core(pos, m, false, 0);
}

bool see_ge(const Position& pos, const Move& m, int threshold) {
#ifdef HOMEI_PROFILE
    SearchProfile& prof = get_search_profile();
    if (prof.enabled) {
        ++prof.see_calls;
        auto t0 = std::chrono::steady_clock::now();
        bool r = stack_see_core(pos, m, true, threshold) >= threshold;
        prof.see_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        return r;
    }
#endif
    return stack_see_core(pos, m, true, threshold) >= threshold;
}

int square_hanging_loss(const Position& pos, int sq) {
    return square_hanging_loss_impl(pos, sq, false, nullptr);
}

int square_hanging_loss_fast(const Position& pos, int sq, const Bitboard bb_att[2]) {
    return square_hanging_loss_impl(pos, sq, true, bb_att);
}

} // namespace homei

#pragma once

#include "bitboard.h"
#include "geometry.h"
#include "types.h"

namespace homei {

inline void init_attack_tables() {
    init_homei_tables();
}

// Fast bitboard check if piece (ptype, facing) placed at `from_sq` attacks `target_sq` on `pos`.
inline bool piece_attacks_sq(const Position& pos, int from_sq, uint8_t ptype, uint8_t facing, int target_sq) {
    if (from_sq < 0 || from_sq >= NUM_SQUARES || target_sq < 0 || target_sq >= NUM_SQUARES || from_sq == target_sq) {
        return false;
    }
    if (ptype == LONG) {
        if (!BB_ORTHO_MASK[from_sq].test_bit(target_sq)) return false;
        return ortho_attacks(from_sq, pos.bb_occ).test_bit(target_sq);
    }
    if (ptype == PHUONG) {
        if (!BB_DIAG_MASK[from_sq].test_bit(target_sq)) return false;
        return diag_attacks(from_sq, pos.bb_occ).test_bit(target_sq);
    }
    if (ptype == XA) {
        uint8_t f = facing & 3;
        if (!BB_RAY_ORTHO[from_sq][f].test_bit(target_sq)) return false;
        return ray_ortho_attack(from_sq, f, pos.bb_occ).test_bit(target_sq);
    }
    return BB_STEP_ATTACKS[ptype][facing & 3][from_sq].test_bit(target_sq);
}

// Fast bitboard reverse-lookup attack check under custom occupancy `occ` (e.g. king removed for king evasion check).
// Uses ONE ortho_attacks call to check both LONG and XA, and skips sliders during step-piece loop.
inline bool is_attacked_with_occ(const Position& pos, int target_sq, Side by_side, const Bitboard& occ) {
    if (target_sq < 0 || target_sq >= NUM_SQUARES) return false;

    const Bitboard side_bb = pos.bb_side[by_side];
    const Bitboard bb_l = side_bb & pos.bb_type[LONG];
    const Bitboard bb_p = side_bb & pos.bb_type[PHUONG];
    const Bitboard bb_x = side_bb & pos.bb_type[XA];

    // 1. Step and jump pieces (V, T, S, K, B) inside 5x5 local zone
    Bitboard step_cands = (side_bb & ~(bb_l | bb_p | bb_x)) & BB_STEP_ZONE[target_sq];
    while (step_cands.any()) {
        int ssq = step_cands.pop_lsb();
        const Piece top = pos.top_piece[ssq];
        if (BB_STEP_ATTACKS[top.type][top.facing][ssq].test_bit(target_sq)) {
            return true;
        }
    }

    // 2. LONG + XA via single branchless ortho_attacks call
    const Bitboard bb_lx = bb_l | bb_x;
    if ((BB_ORTHO_MASK[target_sq] & bb_lx).any()) {
        const Bitboard ortho = ortho_attacks(target_sq, occ);
        if ((ortho & bb_l).any()) return true;
        Bitboard xa_hits = ortho & bb_x;
        while (xa_hits.any()) {
            int xsq = xa_hits.pop_lsb();
            if (BB_RAY_ORTHO[xsq][pos.top_piece[xsq].facing].test_bit(target_sq)) {
                return true;
            }
        }
    }

    // 3. PHUONG via single branchless diag_attacks call
    if ((BB_DIAG_MASK[target_sq] & bb_p).any()) {
        if ((diag_attacks(target_sq, occ) & bb_p).any()) {
            return true;
        }
    }

    return false;
}

inline bool is_attacked(const Position& pos, int target_sq, Side by_side) {
    return is_attacked_with_occ(pos, target_sq, by_side, pos.bb_occ);
}

// Returns bitboard of ALL pieces of `by_side` attacking `target_sq` under occupancy `occ`.
inline Bitboard attackers_to_side_bb(const Position& pos, int target_sq, Side by_side, const Bitboard& occ) {
    Bitboard res;
    if (target_sq < 0 || target_sq >= NUM_SQUARES) return res;

    const Bitboard side_bb = pos.bb_side[by_side] & occ;
    const Bitboard bb_l = side_bb & pos.bb_type[LONG];
    const Bitboard bb_p = side_bb & pos.bb_type[PHUONG];
    const Bitboard bb_x = side_bb & pos.bb_type[XA];

    Bitboard step_cands = (side_bb & ~(bb_l | bb_p | bb_x)) & BB_STEP_ZONE[target_sq];
    while (step_cands.any()) {
        int ssq = step_cands.pop_lsb();
        const Piece top = pos.top_piece[ssq];
        if (BB_STEP_ATTACKS[top.type][top.facing][ssq].test_bit(target_sq)) {
            res.set_bit(ssq);
        }
    }

    if ((BB_ORTHO_MASK[target_sq] & (bb_l | bb_x)).any()) {
        const Bitboard ortho = ortho_attacks(target_sq, occ);
        res |= (ortho & bb_l);
        Bitboard xa_hits = ortho & bb_x;
        while (xa_hits.any()) {
            int xsq = xa_hits.pop_lsb();
            if (BB_RAY_ORTHO[xsq][pos.top_piece[xsq].facing].test_bit(target_sq)) {
                res.set_bit(xsq);
            }
        }
    }

    if ((BB_DIAG_MASK[target_sq] & bb_p).any()) {
        res |= (diag_attacks(target_sq, occ) & bb_p);
    }

    return res;
}

// Returns bitboard of ALL pieces (BOTH sides) attacking `target_sq` under occupancy `occ` in ONE pass.
inline Bitboard attackers_to_both_bb(const Position& pos, int target_sq, const Bitboard& occ) {
    Bitboard res;
    if (target_sq < 0 || target_sq >= NUM_SQUARES) return res;

    const Bitboard bb_l = pos.bb_type[LONG] & occ;
    const Bitboard bb_p = pos.bb_type[PHUONG] & occ;
    const Bitboard bb_x = pos.bb_type[XA] & occ;

    Bitboard step_cands = (occ & ~(bb_l | bb_p | bb_x)) & BB_STEP_ZONE[target_sq];
    while (step_cands.any()) {
        int ssq = step_cands.pop_lsb();
        const Piece top = pos.top_piece[ssq];
        if (BB_STEP_ATTACKS[top.type][top.facing][ssq].test_bit(target_sq)) {
            res.set_bit(ssq);
        }
    }

    if ((BB_ORTHO_MASK[target_sq] & (bb_l | bb_x)).any()) {
        const Bitboard ortho = ortho_attacks(target_sq, occ);
        res |= (ortho & bb_l);
        Bitboard xa_hits = ortho & bb_x;
        while (xa_hits.any()) {
            int xsq = xa_hits.pop_lsb();
            if (BB_RAY_ORTHO[xsq][pos.top_piece[xsq].facing].test_bit(target_sq)) {
                res.set_bit(xsq);
            }
        }
    }

    if ((BB_DIAG_MASK[target_sq] & bb_p).any()) {
        res |= (diag_attacks(target_sq, occ) & bb_p);
    }

    return res;
}

// Computes all pieces that are the SOLE blocker between `king_side`'s King and an enemy slider sniper.
// Populates `out_pinners` with the enemy snipers pinning at least one `king_side` piece.
inline Bitboard slider_blockers(const Position& pos, Side king_side, Bitboard& out_pinners) {
    out_pinners.clear_all();
    int ksq = pos.king_sq[king_side];
    if (ksq >= NUM_SQUARES) return Bitboard{};

    Side enemy = other_side(king_side);
    const Bitboard enemy_bb = pos.bb_side[enemy];
    Bitboard snipers =
        (BB_ORTHO_MASK[ksq] & enemy_bb & (pos.bb_type[LONG] | pos.bb_type[XA])) |
        (BB_DIAG_MASK[ksq]  & enemy_bb & pos.bb_type[PHUONG]);

    Bitboard blockers;
    while (snipers.any()) {
        int ssq = snipers.pop_lsb();
        const Piece sp = pos.top_piece[ssq];
        if (sp.type == XA && !BB_RAY_ORTHO[ssq][sp.facing].test_bit(ksq)) {
            continue;
        }
        Bitboard between = BB_BETWEEN[ksq][ssq] & pos.bb_occ;
        if (between.any() && !between.more_than_one()) {
            blockers |= between;
            if ((between & pos.bb_side[king_side]).any()) {
                out_pinners.set_bit(ssq);
            }
        }
    }
    return blockers;
}

// Returns bitboard of all enemy pieces checking `side`'s VUONG.
inline Bitboard checkers_bb(const Position& pos, Side side) {
    uint8_t ksq = pos.king_sq[side];
    if (ksq >= NUM_SQUARES) return Bitboard{};
    return attackers_to_side_bb(pos, ksq, other_side(side), pos.bb_occ);
}

// Returns true if `side`'s VUONG is currently attacked by `other_side(side)`.
inline bool is_in_check(const Position& pos, Side side) {
    uint8_t ksq = pos.king_sq[side];
    if (ksq >= NUM_SQUARES) {
        Bitboard kb = pos.bb_side[side] & pos.bb_type[VUONG];
        if (kb.any()) {
            return is_attacked(pos, kb.lsb(), other_side(side));
        }
        return false;
    }
    return is_attacked(pos, ksq, other_side(side));
}

// Populates `attacker_sqs` with all squares containing a top piece of `by_side`
// whose raw_destinations include `target_sq`. Returns count of attackers.
int get_attackers(const Position& pos, int target_sq, Side by_side, int attacker_sqs[NUM_SQUARES]);

struct AttackerList {
    int sq[NUM_SQUARES]{};
    int count = 0;
};

inline AttackerList get_attackers(const Position& pos, int target_sq, Side by_side) {
    AttackerList out;
    out.count = get_attackers(pos, target_sq, by_side, out.sq);
    return out;
}

} // namespace homei

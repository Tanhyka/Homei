#pragma once

#include "types.h"
#include "geometry.h"

namespace homei {

struct ReverseAttackEntry {
    uint8_t src_sq = 0;
    uint8_t ptype_mask_by_facing[4]{}; // bitmask of PieceType (1 << pt) for each facing 0..3
};

struct ReverseAttackList {
    uint8_t count = 0;
    ReverseAttackEntry entries[24]{};
};

inline ReverseAttackList g_rev_step_jump[NUM_SQUARES];
inline uint8_t g_ray_ortho[NUM_SQUARES][4][10];
inline uint8_t g_ray_diag[NUM_SQUARES][4][10];
inline bool g_attack_init = false;

void init_attack_tables();

// Fast check if piece (ptype, facing) placed at `from_sq` attacks `target_sq` on `pos`.
inline bool piece_attacks_sq(const Position& pos, int from_sq, uint8_t ptype, uint8_t facing, int target_sq) {
    if (from_sq < 0 || from_sq >= NUM_SQUARES || from_sq == target_sq) return false;
    const PieceMoveTable& tbl = get_piece_moves(ptype, facing);
    int r0 = sq_row(from_sq);
    int c0 = sq_col(from_sq);
    int rt = sq_row(target_sq);
    int ct = sq_col(target_sq);

    for (int i = 0; i < tbl.count; ++i) {
        const MoveVec& mv = tbl.vecs[i];
        if (!mv.sliding) {
            if (r0 + mv.dr == rt && c0 + mv.dc == ct) {
                return true;
            }
        } else {
            int nr = r0 + mv.dr;
            int nc = c0 + mv.dc;
            while (in_bounds(nr, nc)) {
                int s = make_sq(nr, nc);
                if (s == target_sq) return true;
                if (!pos.cell[s].empty()) break;
                nr += mv.dr;
                nc += mv.dc;
            }
        }
    }
    return false;
}

// Fast reverse-lookup attack check: returns true if any top piece of `by_side`
// has `target_sq` as a raw destination (empty, capture, or own).
inline bool is_attacked(const Position& pos, int target_sq, Side by_side) {
    if (!g_attack_init) init_attack_tables();
    if (target_sq < 0 || target_sq >= NUM_SQUARES) return false;

    // 1. Step and jump attacks via reverse lookup
    const auto& list = g_rev_step_jump[target_sq];
    for (uint8_t i = 0; i < list.count; ++i) {
        const auto& e = list.entries[i];
        const Cell& c = pos.cell[e.src_sq];
        if (!c.empty()) {
            const Piece& top = c.top();
            if (top.owner == by_side && (e.ptype_mask_by_facing[top.facing] & (1U << top.type))) {
                return true;
            }
        }
    }

    // 2. 4 orthogonal rays (LONG and XA)
    for (int dir = 0; dir < 4; ++dir) {
        const uint8_t* ray = g_ray_ortho[target_sq][dir];
        for (int step = 0; ray[step] != 255; ++step) {
            const Cell& cell = pos.cell[ray[step]];
            if (!cell.empty()) {
                const Piece& top = cell.top();
                if (top.owner == by_side) {
                    if (top.type == LONG) return true;
                    if (top.type == XA && top.facing == dir) return true;
                }
                break;
            }
        }
    }

    // 3. 4 diagonal rays (PHUONG)
    for (int dir = 0; dir < 4; ++dir) {
        const uint8_t* ray = g_ray_diag[target_sq][dir];
        for (int step = 0; ray[step] != 255; ++step) {
            const Cell& cell = pos.cell[ray[step]];
            if (!cell.empty()) {
                const Piece& top = cell.top();
                if (top.owner == by_side && top.type == PHUONG) {
                    return true;
                }
                break;
            }
        }
    }

    return false;
}

// Returns true if `side`'s VUONG is currently attacked by `other_side(side)`.
inline bool is_in_check(const Position& pos, Side side) {
    uint8_t ksq = pos.king_sq[side];
    if (ksq >= NUM_SQUARES) {
        // Fallback scan if king_sq wasn't cached
        for (int sq = 0; sq < NUM_SQUARES; ++sq) {
            if (!pos.cell[sq].empty() && pos.cell[sq].top().owner == side && pos.cell[sq].top().type == VUONG) {
                return is_attacked(pos, sq, other_side(side));
            }
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

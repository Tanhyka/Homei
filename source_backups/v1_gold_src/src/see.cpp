#include "see.h"
#include "attack.h"
#include "geometry.h"
#include "movegen.h"
#include <algorithm>

namespace homei {

namespace {

// Exchange swing of capturing/losing a piece on the board:
// Capturable pieces (T, S, K, X, B) transfer to the capturer's prisoner hand (~1.85x V),
// whereas Divine Beasts (LONG, PHUONG) vanish when captured (1.0x V).
inline int piece_exchange_swing(uint8_t pt) {
    int v = piece_material_value(pt);
    if (pt == LONG || pt == PHUONG || pt == VUONG) {
        return v;
    }
    return v + (v * 17) / 20; // 1.85 * v
}

inline int cell_exchange_swing(const Cell& c) {
    int sum = 0;
    for (uint8_t i = 0; i < c.size; ++i) {
        sum += piece_exchange_swing(c.stack[i].type);
    }
    return sum;
}

// Checks if piece `p` located at `from_sq` could reach `target_sq` on `pos`
// (assuming `from_sq`'s current top piece has already moved away).
bool second_piece_can_reach(const Position& pos, int from_sq, const Piece& p, int target_sq) {
    if (from_sq == target_sq) return false;
    const PieceMoveTable& tbl = get_piece_moves(p.type, p.facing);
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

} // namespace

int stack_see_estimate(const Position& pos, const Move& m) {
    if (!is_capture_move(pos, m)) return 0;

    const Cell& dst_cell = pos.cell[m.dst];
    const Cell& src_cell = pos.cell[m.src];
    int v_dst = cell_exchange_swing(dst_cell);
    int v_moved = (m.kind == MoveKind::Team)
        ? cell_exchange_swing(src_cell)
        : piece_exchange_swing(src_cell.top().type);

    Side stm = static_cast<Side>(pos.side_to_move);
    Side enemy = other_side(stm);

    Position& mut_pos = const_cast<Position&>(pos);
    UndoState u0;
    apply_move(mut_pos, m, u0);

    // Find cheapest legal enemy recapture onto m.dst
    AttackerList enemy_att = get_attackers(mut_pos, m.dst, enemy);
    int best_enemy_asq = -1;
    int min_enemy_val = 100000;
    for (int i = 0; i < enemy_att.count; ++i) {
        int asq = enemy_att.sq[i];
        const Cell& ac = mut_pos.cell[asq];
        if (ac.empty()) continue;
        Move cap_mv{MoveKind::Single, static_cast<int8_t>(asq), m.dst, ac.top().type, -1, 0};
        UndoState u1;
        apply_move(mut_pos, cap_mv, u1);
        bool legal = !is_in_check(mut_pos, enemy);
        undo_move(mut_pos, cap_mv, u1);
        if (legal) {
            int aval = piece_exchange_swing(ac.top().type);
            if (aval < min_enemy_val) {
                min_enemy_val = aval;
                best_enemy_asq = asq;
            }
        }
    }

    if (best_enemy_asq < 0) {
        undo_move(mut_pos, m, u0);
        return v_dst;
    }

    // Simulate enemy recapture and check if stm has a follow-up recapture
    int net_after_recap = v_dst - v_moved;
    const Cell& eac = mut_pos.cell[best_enemy_asq];
    Move enemy_cap{MoveKind::Single, static_cast<int8_t>(best_enemy_asq), m.dst, eac.top().type, -1, 0};
    UndoState u1;
    apply_move(mut_pos, enemy_cap, u1);

    AttackerList stm_att2 = get_attackers(mut_pos, m.dst, stm);
    int best_stm_asq2 = -1;
    int min_stm_val2 = 100000;
    for (int i = 0; i < stm_att2.count; ++i) {
        int asq = stm_att2.sq[i];
        const Cell& ac = mut_pos.cell[asq];
        if (ac.empty()) continue;
        Move cap_mv2{MoveKind::Single, static_cast<int8_t>(asq), m.dst, ac.top().type, -1, 0};
        UndoState u2;
        apply_move(mut_pos, cap_mv2, u2);
        bool legal = !is_in_check(mut_pos, stm);
        undo_move(mut_pos, cap_mv2, u2);
        if (legal) {
            int aval = piece_exchange_swing(ac.top().type);
            if (aval < min_stm_val2) {
                min_stm_val2 = aval;
                best_stm_asq2 = asq;
            }
        }
    }

    if (best_stm_asq2 >= 0) {
        const Cell& sac2 = mut_pos.cell[best_stm_asq2];
        Move stm_cap2{MoveKind::Single, static_cast<int8_t>(best_stm_asq2), m.dst, sac2.top().type, -1, 0};
        UndoState u2;
        apply_move(mut_pos, stm_cap2, u2);
        bool enemy_has_3rd = is_attacked(mut_pos, m.dst, enemy);
        undo_move(mut_pos, stm_cap2, u2);

        int gain_with_2nd = v_dst - v_moved + min_enemy_val - (enemy_has_3rd ? min_stm_val2 : 0);
        if (gain_with_2nd > net_after_recap) {
            net_after_recap = gain_with_2nd;
        }
    }

    undo_move(mut_pos, enemy_cap, u1);
    undo_move(mut_pos, m, u0);
    return net_after_recap;
}

int square_hanging_loss(const Position& pos, int sq) {
    const Cell& target = pos.cell[sq];
    if (target.empty() || target.contains_king()) return 0;

    Side owner = target.owner();
    Side attacker_side = other_side(owner);
    AttackerList attackers = get_attackers(pos, sq, attacker_side);
    if (attackers.count == 0) return 0;

    int v_target = cell_material_value(target);
    int swing_target = cell_exchange_swing(target);
    AttackerList defenders = get_attackers(pos, sq, owner);

    // Count total attacking and defending units (including 2nd piece in a stack if it also attacks `sq`)
    int total_attack_units = attackers.count;
    int min_attacker_swing = 100000;
    for (int i = 0; i < attackers.count; ++i) {
        int asq = attackers.sq[i];
        const Cell& ac = pos.cell[asq];
        if (ac.empty()) continue;
        int aswing = piece_exchange_swing(ac.top().type);
        if (aswing < min_attacker_swing) {
            min_attacker_swing = aswing;
        }
        if (ac.size >= 2 && second_piece_can_reach(pos, asq, ac.stack[ac.size - 2], sq)) {
            ++total_attack_units;
        }
    }

    int total_defend_units = defenders.count;
    int min_defender_swing = 100000;
    for (int i = 0; i < defenders.count; ++i) {
        int dsq = defenders.sq[i];
        const Cell& dc = pos.cell[dsq];
        if (dc.empty()) continue;
        int dswing = piece_exchange_swing(dc.top().type);
        if (dswing < min_defender_swing) {
            min_defender_swing = dswing;
        }
        if (dc.size >= 2 && second_piece_can_reach(pos, dsq, dc.stack[dc.size - 2], sq)) {
            ++total_defend_units;
        }
    }

    if (total_defend_units == 0) {
        return v_target;
    }
    if (swing_target > min_attacker_swing) {
        return (swing_target - min_attacker_swing) / 2;
    }
    // Check overload: more attacking units than defending units (e.g., 2 attackers vs 1 defender)
    if (total_attack_units > total_defend_units) {
        if (swing_target + min_defender_swing > min_attacker_swing) {
            return (v_target * 2) / 3;
        }
    }
    return 0;
}

} // namespace homei

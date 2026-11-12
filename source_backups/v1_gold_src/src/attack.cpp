#include "attack.h"
#include "geometry.h"

namespace homei {

namespace {

struct ReverseAttackEntry {
    uint8_t src_sq = 0;
    uint8_t ptype_mask_by_facing[4]{}; // bitmask of PieceType (1 << pt) for each facing 0..3
};

struct ReverseAttackList {
    uint8_t count = 0;
    ReverseAttackEntry entries[24]{};
};

ReverseAttackList g_rev_step_jump[NUM_SQUARES];
bool g_attack_init = false;

} // namespace

void init_attack_tables() {
    if (g_attack_init) return;
    init_geometry();

    for (int tsq = 0; tsq < NUM_SQUARES; ++tsq) {
        g_rev_step_jump[tsq].count = 0;
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
                auto& list = g_rev_step_jump[tsq];
                auto& e = list.entries[list.count++];
                e.src_sq = static_cast<uint8_t>(ssq);
                for (int f = 0; f < 4; ++f) {
                    e.ptype_mask_by_facing[f] = masks_for_target[tsq][f];
                }
            }
        }
    }

    g_attack_init = true;
}

bool is_attacked(const Position& pos, int target_sq, Side by_side) {
    if (!g_attack_init) init_attack_tables();
    if (target_sq < 0 || target_sq >= NUM_SQUARES) return false;

    // 1. Check step and jump attackers via reverse lookup table
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

    int tr = sq_row(target_sq);
    int tc = sq_col(target_sq);

    // 2. Check 4 orthogonal rays for LONG and XA
    // Ray dir (dr, dc) from target_sq means an attacker at (r, c) slides in direction (-dr, -dc).
    // XA facing: 0=(-1,0), 1=(0,+1), 2=(+1,0), 3=(0,-1).
    // So attacker at ray (+1,0) (below target) must have XA facing 0 (up) to hit target.
    // Ray (0,-1) (left of target) -> XA facing 1 (right).
    // Ray (-1,0) (above target) -> XA facing 2 (down).
    // Ray (0,+1) (right of target) -> XA facing 3 (left).
    constexpr int ortho_dr[4] = {1, 0, -1, 0};
    constexpr int ortho_dc[4] = {0, -1, 0, 1};
    for (int dir = 0; dir < 4; ++dir) {
        int r = tr + ortho_dr[dir];
        int c = tc + ortho_dc[dir];
        while (in_bounds(r, c)) {
            const Cell& cell = pos.cell[make_sq(r, c)];
            if (!cell.empty()) {
                const Piece& top = cell.top();
                if (top.owner == by_side) {
                    if (top.type == LONG) return true;
                    if (top.type == XA && top.facing == dir) return true;
                }
                break;
            }
            r += ortho_dr[dir];
            c += ortho_dc[dir];
        }
    }

    // 3. Check 4 diagonal rays for PHUONG
    constexpr int diag_dr[4] = {-1, -1, 1, 1};
    constexpr int diag_dc[4] = {-1, 1, -1, 1};
    for (int dir = 0; dir < 4; ++dir) {
        int r = tr + diag_dr[dir];
        int c = tc + diag_dc[dir];
        while (in_bounds(r, c)) {
            const Cell& cell = pos.cell[make_sq(r, c)];
            if (!cell.empty()) {
                const Piece& top = cell.top();
                if (top.owner == by_side && top.type == PHUONG) {
                    return true;
                }
                break;
            }
            r += diag_dr[dir];
            c += diag_dc[dir];
        }
    }

    return false;
}

int get_attackers(const Position& pos, int target_sq, Side by_side, int attacker_sqs[NUM_SQUARES]) {
    if (!g_attack_init) init_attack_tables();
    if (target_sq < 0 || target_sq >= NUM_SQUARES) return 0;

    int count = 0;
    const auto& list = g_rev_step_jump[target_sq];
    for (uint8_t i = 0; i < list.count; ++i) {
        const auto& e = list.entries[i];
        const Cell& c = pos.cell[e.src_sq];
        if (!c.empty()) {
            const Piece& top = c.top();
            if (top.owner == by_side && (e.ptype_mask_by_facing[top.facing] & (1U << top.type))) {
                attacker_sqs[count++] = e.src_sq;
            }
        }
    }

    int tr = sq_row(target_sq);
    int tc = sq_col(target_sq);

    constexpr int ortho_dr[4] = {1, 0, -1, 0};
    constexpr int ortho_dc[4] = {0, -1, 0, 1};
    for (int dir = 0; dir < 4; ++dir) {
        int r = tr + ortho_dr[dir];
        int c = tc + ortho_dc[dir];
        while (in_bounds(r, c)) {
            int sq = make_sq(r, c);
            const Cell& cell = pos.cell[sq];
            if (!cell.empty()) {
                const Piece& top = cell.top();
                if (top.owner == by_side) {
                    if (top.type == LONG || (top.type == XA && top.facing == dir)) {
                        attacker_sqs[count++] = sq;
                    }
                }
                break;
            }
            r += ortho_dr[dir];
            c += ortho_dc[dir];
        }
    }

    constexpr int diag_dr[4] = {-1, -1, 1, 1};
    constexpr int diag_dc[4] = {-1, 1, -1, 1};
    for (int dir = 0; dir < 4; ++dir) {
        int r = tr + diag_dr[dir];
        int c = tc + diag_dc[dir];
        while (in_bounds(r, c)) {
            int sq = make_sq(r, c);
            const Cell& cell = pos.cell[sq];
            if (!cell.empty()) {
                const Piece& top = cell.top();
                if (top.owner == by_side && top.type == PHUONG) {
                    attacker_sqs[count++] = sq;
                }
                break;
            }
            r += diag_dr[dir];
            c += diag_dc[dir];
        }
    }

    return count;
}

} // namespace homei

#include "attack.h"
#include "geometry.h"

namespace homei {

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

    // Precompute orthogonal and diagonal sliding ray square lookups
    constexpr int ortho_dr[4] = {1, 0, -1, 0};
    constexpr int ortho_dc[4] = {0, -1, 0, 1};
    constexpr int diag_dr[4] = {-1, -1, 1, 1};
    constexpr int diag_dc[4] = {-1, 1, -1, 1};

    for (int tsq = 0; tsq < NUM_SQUARES; ++tsq) {
        int tr = sq_row(tsq);
        int tc = sq_col(tsq);

        for (int dir = 0; dir < 4; ++dir) {
            int r = tr + ortho_dr[dir];
            int c = tc + ortho_dc[dir];
            int step = 0;
            while (in_bounds(r, c)) {
                g_ray_ortho[tsq][dir][step++] = static_cast<uint8_t>(make_sq(r, c));
                r += ortho_dr[dir];
                c += ortho_dc[dir];
            }
            g_ray_ortho[tsq][dir][step] = 255;
        }

        for (int dir = 0; dir < 4; ++dir) {
            int r = tr + diag_dr[dir];
            int c = tc + diag_dc[dir];
            int step = 0;
            while (in_bounds(r, c)) {
                g_ray_diag[tsq][dir][step++] = static_cast<uint8_t>(make_sq(r, c));
                r += diag_dr[dir];
                c += diag_dc[dir];
            }
            g_ray_diag[tsq][dir][step] = 255;
        }
    }

    g_attack_init = true;
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

    for (int dir = 0; dir < 4; ++dir) {
        const uint8_t* ray = g_ray_ortho[target_sq][dir];
        for (int step = 0; ray[step] != 255; ++step) {
            int sq = ray[step];
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
        }
    }

    for (int dir = 0; dir < 4; ++dir) {
        const uint8_t* ray = g_ray_diag[target_sq][dir];
        for (int step = 0; ray[step] != 255; ++step) {
            int sq = ray[step];
            const Cell& cell = pos.cell[sq];
            if (!cell.empty()) {
                const Piece& top = cell.top();
                if (top.owner == by_side && top.type == PHUONG) {
                    attacker_sqs[count++] = sq;
                }
                break;
            }
        }
    }

    return count;
}

} // namespace homei

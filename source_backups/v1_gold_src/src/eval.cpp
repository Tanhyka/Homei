#include "eval.h"
#include "attack.h"
#include "geometry.h"
#include "movegen.h"
#include "see.h"
#include "zobrist.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

namespace homei {

namespace {

// Evaluates positional/material terms for one side `s` (excluding turn-dependent hanging terms).
int eval_side_static(const Position& pos, Side s, const bool attacked_by[2][NUM_SQUARES]) {
    Side enemy = other_side(s);
    int score = 0;

    // 1. Hand: Reserve & Prisoners (Section 4.3 item 2: 0.9x for prisoners + global drop flexibility)
    if (pos.reserve[s] & RESERVE_LONG) {
        score += eval_piece_reserve_value(LONG);
    }
    if (pos.reserve[s] & RESERVE_PHUONG) {
        score += eval_piece_reserve_value(PHUONG);
    }
    for (int pt = 1; pt < 8; ++pt) {
        int count = pos.prisoners[s][pt];
        if (count > 0) {
            score += count * eval_piece_prisoner_value(static_cast<uint8_t>(pt));
        }
    }

    int enemy_king_sq = pos.king_sq[enemy];
    int ekr = (enemy_king_sq < NUM_SQUARES) ? sq_row(enemy_king_sq) : -1;
    int ekc = (enemy_king_sq < NUM_SQUARES) ? sq_col(enemy_king_sq) : -1;

    int own_king_sq = pos.king_sq[s];
    int okr = (own_king_sq < NUM_SQUARES) ? sq_row(own_king_sq) : -1;
    int okc = (own_king_sq < NUM_SQUARES) ? sq_col(own_king_sq) : -1;

    uint8_t forward_dir = initial_facing(s);         // CAM: 2 (Down), XANH: 0 (Up)
    uint8_t backward_dir = initial_facing(enemy);    // CAM: 0 (Up),   XANH: 2 (Down)

    // 2. Board squares owned by `s`
    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        const Cell& c = pos.cell[sq];
        if (c.empty() || c.owner() != s) continue;

        int r = sq_row(sq);
        int col = sq_col(sq);

        // Material of all pieces in stack
        for (uint8_t i = 0; i < c.size; ++i) {
            uint8_t pt = c.stack[i].type;
            score += eval_piece_board_value(pt);
        }

        // Stack synergy (Section 4.3 item 3):
        // +10 cp base stack synergy, plus +20 cp if 2nd piece from top (stack[size - 2])
        // is a sliding piece (LONG, PHUONG, XA) aiming a ray toward enemy King.
        if (c.size >= 2) {
            score += 10;
            const Piece& second = c.stack[c.size - 2];
            if (ekr >= 0) {
                bool aims_at_ek = false;
                if (second.type == LONG) {
                    aims_at_ek = (r == ekr || col == ekc);
                } else if (second.type == PHUONG) {
                    aims_at_ek = (std::abs(r - ekr) == std::abs(col - ekc));
                } else if (second.type == XA) {
                    if (second.facing == 0) aims_at_ek = (col == ekc && ekr < r);
                    else if (second.facing == 1) aims_at_ek = (r == ekr && ekc > col);
                    else if (second.facing == 2) aims_at_ek = (col == ekc && ekr > r);
                    else if (second.facing == 3) aims_at_ek = (r == ekr && ekc < col);
                }
                if (aims_at_ek) {
                    score += 20;
                }
            }
        }

        // Orientation & activity of top piece (Section 4.3 item 6)
        const Piece& top = c.top();
        if (top.type != VUONG) {
            int rel_row = (s == CAM) ? r : (8 - r);
            int center_dist = std::abs(col - 4);
            bool sq_enemy_att = attacked_by[enemy][sq];

            if (top.type == BINH) {
                score += rel_row * 3;
                if (top.facing == forward_dir) {
                    score += 6;
                } else if (top.facing == 1 || top.facing == 3) {
                    int c_adj = col + (top.facing == 1 ? 1 : -1);
                    if (in_bounds(r, c_adj)) {
                        int adj_sq = make_sq(r, c_adj);
                        const Cell& adj_c = pos.cell[adj_sq];
                        int r_behind = r + (s == CAM ? -1 : 1);
                        bool guards_long_or_king = false;
                        if (in_bounds(r_behind, c_adj)) {
                            const Cell& bc = pos.cell[make_sq(r_behind, c_adj)];
                            if (!bc.empty() && bc.owner() == s &&
                                (bc.top().type == LONG || bc.top().type == VUONG)) {
                                guards_long_or_king = true;
                            }
                        }
                        if (!adj_c.empty() && adj_c.owner() == s && guards_long_or_king) {
                            score += 14; // Reinforcing pawn directly in front of own Long/King (T17)
                        }
                    }
                } else if (top.facing == backward_dir) {
                    score -= 6;
                }
                // Pawn shield directly in front of or adjacent to own King (e.g. T21 B*b8^)
                if (okr >= 0 && std::abs(r - okr) <= 1 && std::abs(col - okc) <= 1) {
                    if (col == okc && r == okr + (s == CAM ? 1 : -1)) {
                        score += 36;
                    } else {
                        score += 10;
                    }
                }
            } else if (top.type == TUONG || top.type == SI) {
                score += rel_row * 3 + (4 - center_dist) * 2;
                if (okr >= 0 && !sq_enemy_att) {
                    int dist_ok = std::abs(r - okr) + std::abs(col - okc);
                    if (dist_ok == 1) score += 32;
                    else if (dist_ok == 2) score += 12;
                    else if (dist_ok == 3) score += 4;
                }
                const PieceMoveTable& tbl = get_piece_moves(top.type, top.facing);
                for (int vi = 0; vi < tbl.count; ++vi) {
                    int tr = r + tbl.vecs[vi].dr;
                    int tc = col + tbl.vecs[vi].dc;
                    if (in_bounds(tr, tc)) {
                        int tsq = make_sq(tr, tc);
                        if (!pos.cell[tsq].empty()) {
                            if (pos.cell[tsq].owner() == enemy) {
                                score += 12;
                            } else if (top.facing == forward_dir && pos.cell[tsq].top().type == BINH) {
                                score += 6;
                            }
                        }
                    }
                }
                if (ekr >= 0 && (std::abs(r - ekr) + std::abs(col - ekc)) <= 3) {
                    int dr_ek = std::abs(r - ekr);
                    int dc_ek = std::abs(col - ekc);
                    bool primary_ek = (dr_ek >= dc_ek)
                        ? ((top.facing == 0 && ekr < r) || (top.facing == 2 && ekr > r))
                        : ((top.facing == 1 && ekc > col) || (top.facing == 3 && ekc < col));
                    bool secondary_ek =
                        (top.facing == 0 && ekr < r) ||
                        (top.facing == 2 && ekr > r) ||
                        (top.facing == 1 && ekc > col) ||
                        (top.facing == 3 && ekc < col);
                    if (primary_ek) score += 14;
                    else if (secondary_ek) score += 4;
                } else {
                    if (top.facing == forward_dir) score += 6;
                    else if (top.facing == backward_dir) score -= 6;
                }
            } else if (top.type == KI) {
                score += rel_row * 3 + (4 - center_dist) * 2;
                const PieceMoveTable& tbl = get_piece_moves(KI, top.facing);
                for (int vi = 0; vi < tbl.count; ++vi) {
                    int tr = r + tbl.vecs[vi].dr;
                    int tc = col + tbl.vecs[vi].dc;
                    if (in_bounds(tr, tc)) {
                        int tsq = make_sq(tr, tc);
                        if (!pos.cell[tsq].empty() && pos.cell[tsq].owner() == enemy) {
                            score += 12;
                        }
                    }
                }
            } else if (top.type == LONG || top.type == PHUONG) {
                score += (4 - center_dist) * 2;
                if (rel_row >= 3 && rel_row <= 5) {
                    score += 18; // Active central ranks in front of own pawn wall (T18-T20)
                    if (rel_row == 4) score += 4;
                }
            } else if (top.type == XA) {
                score += (4 - center_dist) * 2;
                if (top.facing == forward_dir) score += 6;
                else if (top.facing == backward_dir) score -= 6;
            }

            // Exposure risk when a non-pawn top piece sits on an enemy-attacked square
            if (top.type != BINH && sq_enemy_att) {
                score -= piece_material_value(top.type) / 14;
            }
        }
    }

    // 5. King Safety (Section 4.3 item 5)
    int enemy_prisoners = 0;
    for (int pt = 1; pt < 8; ++pt) {
        enemy_prisoners += pos.prisoners[enemy][pt];
    }
    if (okr >= 0) {
        if (okr == (s == CAM ? 0 : 8)) {
            score += 20; // Keep King safely on home back rank
        }
        int eff_prisoners = std::min(enemy_prisoners, 4);
        int shield_count = 0;
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if (dr == 0 && dc == 0) continue;
                int nr = okr + dr;
                int nc = okc + dc;
                if (!in_bounds(nr, nc)) continue;
                int nsq = make_sq(nr, nc);
                bool enemy_att = attacked_by[enemy][nsq];
                const Cell& nc_cell = pos.cell[nsq];
                if (!nc_cell.empty() && nc_cell.owner() == s) {
                    int nr_rel = (s == CAM) ? nr : (8 - nr);
                    if (nr_rel <= 1 && shield_count < 3) {
                        score += 16;
                        ++shield_count;
                    }
                    if (enemy_att && !attacked_by[s][nsq]) {
                        score -= 10;
                    }
                } else {
                    if (enemy_att) {
                        score -= 20;
                        if (enemy_prisoners >= 2) {
                            score -= 25 * eff_prisoners;
                        } else if (nc_cell.empty() && enemy_prisoners >= 1) {
                            score -= 18;
                        }
                    } else if (nc_cell.empty() && enemy_prisoners >= 2) {
                        score -= 5 * eff_prisoners;
                    }
                }
            }
        }
    }

    return score;
}

} // namespace

int evaluate(const Position& pos) {
    Side stm = static_cast<Side>(pos.side_to_move);
    Side ntm = other_side(stm);

    // Precompute attack map for both sides in a single pass over occupied squares
    bool attacked_by[2][NUM_SQUARES]{};
    int dsts[32];
    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        const Cell& c = pos.cell[sq];
        if (c.empty()) continue;
        Side owner = c.owner();
        int n = get_raw_destinations(pos, sq, dsts);
        for (int i = 0; i < n; ++i) {
            attacked_by[owner][dsts[i]] = true;
        }
    }

    int score = eval_side_static(pos, stm, attacked_by) - eval_side_static(pos, ntm, attacked_by);

    // 4. Hanging pieces (Section 4.3 item 4):
    // Track top 2 hanging losses for ntm and stm to avoid odd/even horizon oscillation
    // while accurately scoring single captures (ntm_max1) and forks (stm_max2 / ntm_max2).
    int ntm_max1 = 0, ntm_max2 = 0;
    int stm_max1 = 0, stm_max2 = 0;
    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        const Cell& c = pos.cell[sq];
        if (c.empty() || c.contains_king()) continue;
        Side owner = c.owner();
        Side att_side = other_side(owner);
        if (!attacked_by[att_side][sq]) continue;

        int loss = square_hanging_loss(pos, sq);
        if (loss <= 0) continue;

        if (owner == ntm) {
            if (loss >= ntm_max1) {
                ntm_max2 = ntm_max1;
                ntm_max1 = loss;
            } else if (loss > ntm_max2) {
                ntm_max2 = loss;
            }
        } else {
            if (loss >= stm_max1) {
                stm_max2 = stm_max1;
                stm_max1 = loss;
            } else if (loss > stm_max2) {
                stm_max2 = loss;
            }
        }
    }

    score += (ntm_max1 * 3) / 4 + (ntm_max2 / 4);
    score -= (stm_max1 / 4) + (stm_max2 * 3) / 4;

    return score;
}

int evaluate_for_side(const Position& pos, Side perspective) {
    int s = evaluate(pos);
    return (static_cast<Side>(pos.side_to_move) == perspective) ? s : -s;
}

Position mirror_horizontal(const Position& pos) {
    Position out;
    out.clear();
    out.side_to_move = pos.side_to_move;
    out.reserve[0] = pos.reserve[0];
    out.reserve[1] = pos.reserve[1];
    std::memcpy(out.prisoners, pos.prisoners, sizeof(pos.prisoners));

    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            int src_sq = make_sq(r, c);
            int dst_sq = make_sq(r, BOARD_SIZE - 1 - c);
            const Cell& sc = pos.cell[src_sq];
            Cell& dc = out.cell[dst_sq];
            dc.size = sc.size;
            for (uint8_t i = 0; i < sc.size; ++i) {
                Piece p = sc.stack[i];
                if (can_rotate(p.type)) {
                    if (p.facing == 1) p.facing = 3;
                    else if (p.facing == 3) p.facing = 1;
                }
                dc.stack[i] = p;
                if (p.type == VUONG) {
                    out.king_sq[p.owner] = static_cast<uint8_t>(dst_sq);
                }
            }
        }
    }
    out.zobrist = compute_zobrist(out);
    return out;
}

Position flip_colors_180(const Position& pos) {
    Position out;
    out.clear();
    out.side_to_move = other_side(static_cast<Side>(pos.side_to_move));
    out.reserve[CAM] = pos.reserve[XANH];
    out.reserve[XANH] = pos.reserve[CAM];
    for (int pt = 0; pt < 8; ++pt) {
        out.prisoners[CAM][pt] = pos.prisoners[XANH][pt];
        out.prisoners[XANH][pt] = pos.prisoners[CAM][pt];
    }

    for (int r = 0; r < BOARD_SIZE; ++r) {
        for (int c = 0; c < BOARD_SIZE; ++c) {
            int src_sq = make_sq(r, c);
            int dst_sq = make_sq(BOARD_SIZE - 1 - r, BOARD_SIZE - 1 - c);
            const Cell& sc = pos.cell[src_sq];
            Cell& dc = out.cell[dst_sq];
            dc.size = sc.size;
            for (uint8_t i = 0; i < sc.size; ++i) {
                Piece p = sc.stack[i];
                p.owner = other_side(static_cast<Side>(p.owner));
                if (can_rotate(p.type)) {
                    p.facing = static_cast<uint8_t>((p.facing + 2) & 3);
                } else {
                    p.facing = initial_facing(static_cast<Side>(p.owner));
                }
                dc.stack[i] = p;
                if (p.type == VUONG) {
                    out.king_sq[p.owner] = static_cast<uint8_t>(dst_sq);
                }
            }
        }
    }
    out.zobrist = compute_zobrist(out);
    return out;
}

void print_piecevalues(std::ostream& os) {
    os << "piecevalues begin\n";
    os << "source see.h::piece_material_value\n";

    struct PieceMeta {
        char code;
        const char* name;
        uint8_t pt;
    };
    static const PieceMeta PIECES[8] = {
        {'V', "VUONG",  VUONG},
        {'T', "TUONG",  TUONG},
        {'S', "SI",     SI},
        {'K', "KI",     KI},
        {'X', "XA",     XA},
        {'L', "LONG",   LONG},
        {'P', "PHUONG", PHUONG},
        {'B', "BINH",   BINH},
    };

    for (const auto& p : PIECES) {
        os << "piece code=" << p.code << " name=" << p.name << " board=" << eval_piece_board_value(p.pt) << " reserve=";
        int r_val = eval_piece_reserve_value(p.pt);
        if (r_val >= 0) os << r_val; else os << '-';
        os << " prisoner=";
        int pr_val = eval_piece_prisoner_value(p.pt);
        if (pr_val >= 0) os << pr_val; else os << '-';
        os << '\n';
    }

    os << "start_side_total " << eval_start_side_total() << '\n';
    os << "note orientation, positional rank/file, stack synergy, king safety, hanging piece penalty\n";
    os << "piecevalues end\n";
}

} // namespace homei

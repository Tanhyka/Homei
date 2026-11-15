#include "eval.h"
#include "attack.h"
#include "features.h"
#include "geometry.h"
#include "movegen.h"
#include "see.h"
#include "zobrist.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace homei {

namespace {

static const char* const TERM_NAMES[EVAL_TERM_COUNT] = {
    "stack_base",
    "stack_battery",
    "binh_rel_row",
    "binh_forward",
    "binh_guard_long_king",
    "binh_king_shield_front",
    "binh_king_shield_side",
    "tuong_si_ki_rel_row",
    "center_col_weight",
    "step_attack_enemy",
    "long_phuong_active_rank",
    "xa_forward",
    "koma_dead_all_out",
    "koma_dead_part_vec",
    "koma_dead_binh_home",
    "koma_buried_home",
    "koma_buried_no_ray",
    "koma_mobile_battery",
    "koma_slider_mob_base",
    "koma_slider_mob_prime",
    "koma_xa_rel_row",
    "koma_undeveloped_ki_si"
};

EvalParams g_eval_params = EvalParams::defaults();
bool g_eval_params_initialized = false;

bool parse_params_stream(std::istream& is, EvalParams& out) {
    std::string line;
    while (std::getline(is, line)) {
        size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line.resize(comment_pos);
        }
        for (char& ch : line) {
            if (ch == '=' || ch == ':' || ch == ',') ch = ' ';
        }
        std::istringstream iss(line);
        std::string name;
        int val = 0;
        if (!(iss >> name >> val)) continue;
        for (int i = 0; i < EVAL_TERM_COUNT; ++i) {
            if (name == TERM_NAMES[i]) {
                out.w[i] = val;
                break;
            }
        }
    }
    return true;
}

void ensure_eval_params_loaded() {
    if (g_eval_params_initialized) return;
    g_eval_params = EvalParams::defaults();
    const char* env_path = std::getenv("HOMEI_PARAMS");
    if (env_path && env_path[0] != '\0') {
        std::ifstream ifs(env_path);
        if (ifs.is_open()) {
            parse_params_stream(ifs, g_eval_params);
        }
    }
    g_eval_params_initialized = true;
}

inline bool slider_aims_at_ek(int r, int col, const Piece& second, int ekr, int ekc) {
    if (ekr < 0 || ekc < 0) return false;
    if (second.type == LONG) {
        return (r == ekr || col == ekc);
    } else if (second.type == PHUONG) {
        return (std::abs(r - ekr) == std::abs(col - ekc));
    } else if (second.type == XA) {
        if (second.facing == 0) return (col == ekc && ekr < r);
        if (second.facing == 1) return (r == ekr && ekc > col);
        if (second.facing == 2) return (col == ekc && ekr > r);
        if (second.facing == 3) return (r == ekr && ekc < col);
    }
    return false;
}

// Step 1b: Returns true if from `sq`, along valid move directions of `slider` (LONG, PHUONG, XA),
// passing through ONLY empty squares, the ray reaches at least one square in the 3x3 zone around enemy King.
inline bool has_siege_ray(const Position& pos, int sq, const Piece& slider, int ekr, int ekc) {
    if (ekr < 0 || ekc < 0) return false;
    int r = sq_row(sq);
    int col = sq_col(sq);
    const PieceMoveTable& tbl = get_piece_moves(slider.type, slider.facing);
    for (int i = 0; i < tbl.count; ++i) {
        const MoveVec& mv = tbl.vecs[i];
        if (!mv.sliding) continue;
        int tr = r + mv.dr;
        int tc = col + mv.dc;
        while (in_bounds(tr, tc)) {
            int tsq = make_sq(tr, tc);
            bool in_king_zone = (std::abs(tr - ekr) <= 1 && std::abs(tc - ekc) <= 1);
            if (pos.cell[tsq].empty()) {
                if (in_king_zone) {
                    return true;
                }
            } else {
                if (tr == ekr && tc == ekc) {
                    return true;
                }
                break;
            }
            tr += mv.dr;
            tc += mv.dc;
        }
    }
    return false;
}

struct DeadVecInfo {
    int8_t in_board;
    int8_t out_board;
};

const DeadVecInfo (&get_dead_vec_table())[8][4][NUM_SQUARES] {
    static DeadVecInfo table[8][4][NUM_SQUARES]{};
    static bool inited = false;
    if (!inited) {
        for (int pt = 0; pt < 8; ++pt) {
            for (int f = 0; f < 4; ++f) {
                const PieceMoveTable& dtbl = get_piece_moves(static_cast<uint8_t>(pt), static_cast<uint8_t>(f));
                for (int sq = 0; sq < NUM_SQUARES; ++sq) {
                    int r = sq_row(sq);
                    int col = sq_col(sq);
                    int in_b = 0;
                    for (int vi = 0; vi < dtbl.count; ++vi) {
                        if (in_bounds(r + dtbl.vecs[vi].dr, col + dtbl.vecs[vi].dc)) {
                            ++in_b;
                        }
                    }
                    table[pt][f][sq].in_board = static_cast<int8_t>(in_b);
                    table[pt][f][sq].out_board = static_cast<int8_t>(dtbl.count - in_b);
                }
            }
        }
        inited = true;
    }
    return table;
}

void eval_side_breakdown(
    const Position& pos,
    Side s,
    const bool attacked_by[2][NUM_SQUARES],
    bool koma_on,
    int& fixed_score,
    int feat[EVAL_TERM_COUNT]
) {
    Side enemy = other_side(s);
    fixed_score = 0;

    // 1. Hand: Reserve & Prisoners (Section 4.3 item 2)
    if (pos.reserve[s] & RESERVE_LONG) {
        fixed_score += eval_piece_reserve_value(LONG);
    }
    if (pos.reserve[s] & RESERVE_PHUONG) {
        fixed_score += eval_piece_reserve_value(PHUONG);
    }
    for (int pt = 1; pt < 8; ++pt) {
        int count = pos.prisoners[s][pt];
        if (count > 0) {
            fixed_score += count * eval_piece_prisoner_value(static_cast<uint8_t>(pt));
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

    int developed_units = 0;
    int backrank_ki_si_count = 0;
    if (koma_on) {
        if (!(pos.reserve[s] & RESERVE_LONG)) ++developed_units;
        if (!(pos.reserve[s] & RESERVE_PHUONG)) ++developed_units;
    }

    // 2. Board squares owned by `s`
    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        const Cell& c = pos.cell[sq];
        if (c.empty() || c.owner() != s) continue;

        int r = sq_row(sq);
        int col = sq_col(sq);
        int rel_row = (s == CAM) ? r : (8 - r);

        // Material of all pieces in stack
        for (uint8_t i = 0; i < c.size; ++i) {
            uint8_t pt = c.stack[i].type;
            fixed_score += eval_piece_board_value(pt);
        }

        const Piece& top = c.top();

        // Stack synergy & Step 1b (Buried slider and battery)
        if (c.size >= 2) {
            feat[TERM_STACK_BASE] += 1;
            const Piece& second = c.stack[c.size - 2];
            bool second_is_slider = (second.type == LONG || second.type == PHUONG || second.type == XA);
            if (!koma_on) {
                if (second_is_slider && slider_aims_at_ek(r, col, second, ekr, ekc)) {
                    feat[TERM_STACK_BATTERY] += 1;
                }
            } else if (second_is_slider) {
                bool siege_ray = has_siege_ray(pos, sq, second, ekr, ekc);
                bool top_is_short = (top.type == BINH || top.type == SI || top.type == TUONG);
                if (top_is_short) {
                    if (rel_row <= 3) {
                        feat[TERM_KOMA_BURIED_HOME] -= 1;
                    } else if (!siege_ray) {
                        feat[TERM_KOMA_BURIED_NO_RAY] -= 1;
                    } else {
                        feat[TERM_STACK_BATTERY] += 1;
                    }
                } else {
                    if (slider_aims_at_ek(r, col, second, ekr, ekc) || siege_ray) {
                        feat[TERM_STACK_BATTERY] += 1;
                    }
                    if (siege_ray) {
                        feat[TERM_KOMA_MOBILE_BATTERY] += 1;
                    }
                }
            }
        }

        // Orientation & activity of top piece (Section 4.3 item 6 + Step 1a, 1c, 1d)
        if (top.type != VUONG) {
            int center_dist = std::abs(col - 4);
            bool sq_enemy_att = attacked_by[enemy][sq];

            if (koma_on) {
                if (top.type != BINH) {
                    if (rel_row >= 1) ++developed_units;
                    else if (top.type == KI || top.type == SI) ++backrank_ki_si_count;
                } else if (rel_row >= 3 || c.size >= 2) {
                    ++developed_units;
                }

                // Step 1a: Dead-facing penalty for BINH, KI, SI, XA (without using ply)
                if (top.type == BINH || top.type == KI || top.type == SI || top.type == XA) {
                    const DeadVecInfo& dvi = get_dead_vec_table()[top.type][top.facing][sq];
                    if (dvi.in_board == 0) {
                        feat[TERM_KOMA_DEAD_ALL_OUT] -= 1;
                    }
                    if (dvi.out_board > 0) {
                        feat[TERM_KOMA_DEAD_PART_VEC] -= dvi.out_board;
                    }
                    if (top.type == BINH && rel_row <= 3 && (dvi.in_board == 0 || top.facing == backward_dir)) {
                        feat[TERM_KOMA_DEAD_BINH_HOME] -= 1;
                    }
                }
            }

            if (top.type == BINH) {
                feat[TERM_BINH_REL_ROW] += rel_row;
                if (top.facing == forward_dir) {
                    feat[TERM_BINH_FORWARD] += 1;
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
                            feat[TERM_BINH_GUARD_LONG_KING] += 1;
                        }
                    }
                } else if (top.facing == backward_dir) {
                    feat[TERM_BINH_FORWARD] -= 1;
                }
                if (okr >= 0 && std::abs(r - okr) <= 1 && std::abs(col - okc) <= 1) {
                    if (col == okc && r == okr + (s == CAM ? 1 : -1)) {
                        feat[TERM_BINH_KING_SHIELD_FRONT] += 1;
                    } else {
                        feat[TERM_BINH_KING_SHIELD_SIDE] += 1;
                    }
                }
            } else if (top.type == TUONG || top.type == SI) {
                feat[TERM_TUONG_SI_KI_REL_ROW] += rel_row;
                feat[TERM_CENTER_COL_WEIGHT] += (4 - center_dist);
                if (okr >= 0 && !sq_enemy_att) {
                    int dist_ok = std::abs(r - okr) + std::abs(col - okc);
                    if (dist_ok == 1) fixed_score += 32;
                    else if (dist_ok == 2) fixed_score += 12;
                    else if (dist_ok == 3) fixed_score += 4;
                }
                const PieceMoveTable& tbl = get_piece_moves(top.type, top.facing);
                for (int vi = 0; vi < tbl.count; ++vi) {
                    int tr = r + tbl.vecs[vi].dr;
                    int tc = col + tbl.vecs[vi].dc;
                    if (in_bounds(tr, tc)) {
                        int tsq = make_sq(tr, tc);
                        if (!pos.cell[tsq].empty()) {
                            if (pos.cell[tsq].owner() == enemy) {
                                feat[TERM_STEP_ATTACK_ENEMY] += 1;
                            } else if (top.facing == forward_dir && pos.cell[tsq].top().type == BINH) {
                                fixed_score += 6;
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
                    if (primary_ek) fixed_score += 14;
                    else if (secondary_ek) fixed_score += 4;
                } else {
                    if (top.facing == forward_dir) fixed_score += 6;
                    else if (top.facing == backward_dir) fixed_score -= 6;
                }
            } else if (top.type == KI) {
                feat[TERM_TUONG_SI_KI_REL_ROW] += rel_row;
                feat[TERM_CENTER_COL_WEIGHT] += (4 - center_dist);
                const PieceMoveTable& tbl = get_piece_moves(KI, top.facing);
                for (int vi = 0; vi < tbl.count; ++vi) {
                    int tr = r + tbl.vecs[vi].dr;
                    int tc = col + tbl.vecs[vi].dc;
                    if (in_bounds(tr, tc)) {
                        int tsq = make_sq(tr, tc);
                        if (!pos.cell[tsq].empty() && pos.cell[tsq].owner() == enemy) {
                            feat[TERM_STEP_ATTACK_ENEMY] += 1;
                        }
                    }
                }
            } else if (top.type == LONG || top.type == PHUONG) {
                feat[TERM_CENTER_COL_WEIGHT] += (4 - center_dist);
                if (rel_row >= 3 && rel_row <= 5) {
                    feat[TERM_LONG_PHUONG_ACTIVE_RANK] += 1;
                    if (rel_row == 4) fixed_score += 4;
                }
            } else if (top.type == XA) {
                feat[TERM_CENTER_COL_WEIGHT] += (4 - center_dist);
                if (top.facing == forward_dir) feat[TERM_XA_FORWARD] += 1;
                else if (top.facing == backward_dir) feat[TERM_XA_FORWARD] -= 1;
                if (koma_on) {
                    feat[TERM_KOMA_XA_REL_ROW] += rel_row;
                }
            }

            // Exposure risk when a non-pawn top piece sits on an enemy-attacked square
            if (top.type != BINH && sq_enemy_att) {
                fixed_score -= piece_material_value(top.type) / 14;
            }
        }
    }

    // Step 1d: Undeveloped KI/SI penalty on back rank when most other pieces have developed
    if (koma_on && developed_units >= 4 && backrank_ki_si_count > 0) {
        feat[TERM_KOMA_UNDEVELOPED_KI_SI] -= backrank_ki_si_count;
    }

    // 5. King Safety (Section 4.3 item 5)
    int enemy_prisoners = 0;
    for (int pt = 1; pt < 8; ++pt) {
        enemy_prisoners += pos.prisoners[enemy][pt];
    }
    if (okr >= 0) {
        if (okr == (s == CAM ? 0 : 8)) {
            fixed_score += 20;
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
                        fixed_score += 16;
                        ++shield_count;
                    }
                    if (enemy_att && !attacked_by[s][nsq]) {
                        fixed_score -= 10;
                    }
                } else {
                    if (enemy_att) {
                        fixed_score -= 20;
                        if (enemy_prisoners >= 2) {
                            fixed_score -= 25 * eff_prisoners;
                        } else if (nc_cell.empty() && enemy_prisoners >= 1) {
                            fixed_score -= 18;
                        }
                    } else if (nc_cell.empty() && enemy_prisoners >= 2) {
                        fixed_score -= 5 * eff_prisoners;
                    }
                }
            }
        }
    }
}

} // namespace

const char* eval_term_name(int term_id) {
    if (term_id >= 0 && term_id < EVAL_TERM_COUNT) {
        return TERM_NAMES[term_id];
    }
    return "unknown";
}

const EvalParams& get_eval_params() {
    ensure_eval_params_loaded();
    return g_eval_params;
}

bool load_eval_params_file(const std::string& path) {
    ensure_eval_params_loaded();
    if (path.empty() || path == "default") {
        g_eval_params = EvalParams::defaults();
        return true;
    }
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;
    EvalParams tmp = EvalParams::defaults();
    parse_params_stream(ifs, tmp);
    g_eval_params = tmp;
    return true;
}

EvalBreakdown evaluate_breakdown(const Position& pos) {
    ensure_eval_params_loaded();
    const EvalParams& params = g_eval_params;
    bool koma_on = EngineFeatures::get().koma_eval;

    Side stm = static_cast<Side>(pos.side_to_move);
    Side ntm = other_side(stm);

    bool attacked_by[2][NUM_SQUARES]{};
    int fixed_stm = 0, fixed_ntm = 0;
    int feat_stm[EVAL_TERM_COUNT]{};
    int feat_ntm[EVAL_TERM_COUNT]{};

    int dsts[32];
    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        const Cell& c = pos.cell[sq];
        if (c.empty()) continue;
        Side owner = c.owner();
        int n = get_raw_destinations(pos, sq, dsts);
        uint8_t top_t = c.top().type;
        if (koma_on && (top_t == LONG || top_t == PHUONG)) {
            int mob_base = 0;
            int mob_prime = 0;
            for (int i = 0; i < n; ++i) {
                int tsq = dsts[i];
                attacked_by[owner][tsq] = true;
                const Cell& tc_cell = pos.cell[tsq];
                if (tc_cell.empty() || tc_cell.owner() != owner) {
                    ++mob_base;
                    int tr = sq_row(tsq);
                    int tc = sq_col(tsq);
                    int rel_tr = (owner == CAM) ? tr : (8 - tr);
                    if (rel_tr >= 5 || (tr >= 2 && tr <= 6 && tc >= 2 && tc <= 6)) {
                        ++mob_prime;
                    }
                }
            }
            int* f_arr = (owner == stm) ? feat_stm : feat_ntm;
            f_arr[TERM_KOMA_SLIDER_MOB_BASE] += std::min(12, mob_base);
            f_arr[TERM_KOMA_SLIDER_MOB_PRIME] += std::min(8, mob_prime);
        } else {
            for (int i = 0; i < n; ++i) {
                attacked_by[owner][dsts[i]] = true;
            }
        }
    }

    eval_side_breakdown(pos, stm, attacked_by, koma_on, fixed_stm, feat_stm);
    eval_side_breakdown(pos, ntm, attacked_by, koma_on, fixed_ntm, feat_ntm);

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

    int hanging_score = ((ntm_max1 * 3) / 4 + (ntm_max2 / 4)) - ((stm_max1 / 4) + (stm_max2 * 3) / 4);

    EvalBreakdown out;
    out.base_fixed = (fixed_stm - fixed_ntm) + hanging_score;
    out.total = out.base_fixed;
    for (int i = 0; i < EVAL_TERM_COUNT; ++i) {
        out.raw[i] = feat_stm[i] - feat_ntm[i];
        out.weight[i] = params.w[i];
        out.contrib[i] = out.raw[i] * out.weight[i];
        out.total += out.contrib[i];
    }
    return out;
}

int evaluate(const Position& pos) {
    return evaluate_breakdown(pos).total;
}

void print_evalterms(const Position& pos, std::ostream& os) {
    EvalBreakdown bd = evaluate_breakdown(pos);
    os << "evalterms begin\n";
    os << "term name=base_fixed raw=" << bd.base_fixed << " weight=1 contrib=" << bd.base_fixed << '\n';
    for (int i = 0; i < EVAL_TERM_COUNT; ++i) {
        os << "term name=" << TERM_NAMES[i]
           << " raw=" << bd.raw[i]
           << " weight=" << bd.weight[i]
           << " contrib=" << bd.contrib[i] << '\n';
    }
    os << "total " << bd.total << '\n';
    os << "evalterms end\n";
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

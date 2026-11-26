#include "eval.h"
#include "attack.h"
#include "bitboard.h"
#include "eval_v35.h"
#include "features.h"
#include "geometry.h"
#include "movegen.h"
#include "see.h"
#include "zobrist.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>

namespace homei {

SearchProfile& get_search_profile() {
    static SearchProfile prof{};
    return prof;
}

DeadVecInfo g_dead_vec_table[8][4][NUM_SQUARES]{};

namespace {

struct EvalSqEntry {
    int16_t base;
    int16_t mg;
    int16_t eg;
    int16_t _pad;
};

EvalSqEntry g_sq_table[2][8][4][NUM_SQUARES]{};
EngineFeatures g_eval_feats{};
bool g_eval_fast_eligible = false;
int16_t g_mat_mg[8]{};
int16_t g_mat_eg[8]{};
int16_t g_pris_mg[8]{};
int16_t g_pris_eg[8]{};

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
    "koma_undeveloped_ki_si",
    "siege_units_linear",
    "siege_units_quad",
    "siege_drop_hole",
    "siege_vanguard_support"
};

EvalParams g_eval_params = EvalParams::defaults();
std::once_flag g_eval_params_once;

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

void rebuild_eval_sq_table() {
    g_mat_mg[0] = 0; g_mat_eg[0] = 0;
    g_pris_mg[0] = 0; g_pris_eg[0] = 0;
    for (int pt = 1; pt < 8; ++pt) {
        g_mat_mg[pt] = v35::V35_STRUCT_MG[pt - 1];
        g_mat_eg[pt] = v35::V35_STRUCT_EG[pt - 1];
        g_pris_mg[pt] = v35::V35_STRUCT_MG[v35::S_PRIS_TUONG + (pt - 1)];
        g_pris_eg[pt] = v35::V35_STRUCT_EG[v35::S_PRIS_TUONG + (pt - 1)];
    }

    const int* w = g_eval_params.w;
    for (int si = 0; si < 2; ++si) {
        Side s = static_cast<Side>(si);
        Side enemy = other_side(s);
        uint8_t fwd = initial_facing(s);
        uint8_t bwd = initial_facing(enemy);
        for (int pt = 0; pt < 8; ++pt) {
            for (int f = 0; f < 4; ++f) {
                for (int sq = 0; sq < NUM_SQUARES; ++sq) {
                    int r = sq_row(sq);
                    int col = sq_col(sq);
                    int rel_row = (s == CAM) ? r : (8 - r);
                    int center_dist = std::abs(col - 4);
                    int base = eval_piece_board_value(static_cast<uint8_t>(pt));

                    if (pt != VUONG) {
                        if (pt == BINH || pt == KI || pt == SI || pt == XA) {
                            const DeadVecInfo& dvi = g_dead_vec_table[pt][f][sq];
                            if (dvi.in_board == 0) base -= w[TERM_KOMA_DEAD_ALL_OUT];
                            if (dvi.out_board > 0) base -= static_cast<int>(dvi.out_board) * w[TERM_KOMA_DEAD_PART_VEC];
                            if (pt == BINH && rel_row <= 3 && (dvi.in_board == 0 || f == bwd)) {
                                base -= w[TERM_KOMA_DEAD_BINH_HOME];
                            }
                        }
                        if (pt == BINH) {
                            base += rel_row * w[TERM_BINH_REL_ROW];
                            if (f == fwd) base += w[TERM_BINH_FORWARD];
                            else if (f == bwd) base -= w[TERM_BINH_FORWARD];
                        } else if (pt == TUONG || pt == SI || pt == KI) {
                            base += rel_row * w[TERM_TUONG_SI_KI_REL_ROW];
                            base += (4 - center_dist) * w[TERM_CENTER_COL_WEIGHT];
                        } else if (pt == LONG || pt == PHUONG) {
                            base += (4 - center_dist) * w[TERM_CENTER_COL_WEIGHT];
                            if (rel_row >= 3) {
                                base += w[TERM_LONG_PHUONG_ACTIVE_RANK];
                                if (rel_row == 4) base += 4;
                            }
                        } else if (pt == XA) {
                            base += (4 - center_dist) * w[TERM_CENTER_COL_WEIGHT];
                            if (f == fwd) base += w[TERM_XA_FORWARD];
                            else if (f == bwd) base -= w[TERM_XA_FORWARD];
                            base += rel_row * w[TERM_KOMA_XA_REL_ROW];
                        }
                    }

                    int sc = v35::sym_pst_col(col);
                    int sf = v35::sym_pst_facing(s, col, static_cast<uint8_t>(pt), static_cast<uint8_t>(f));
                    int sq45 = rel_row * 5 + sc;
                    int mg = static_cast<int>(v35::V35_PST_MG[pt][sf][sq45]) + static_cast<int>(g_mat_mg[pt]);
                    int eg = static_cast<int>(v35::V35_PST_EG[pt][sf][sq45]) + static_cast<int>(g_mat_eg[pt]);
                    g_sq_table[si][pt][f][sq] = {
                        static_cast<int16_t>(base),
                        static_cast<int16_t>(mg),
                        static_cast<int16_t>(eg),
                        0
                    };
                }
            }
        }
    }
}

void ensure_eval_params_loaded() {
    std::call_once(g_eval_params_once, []() {
        init_geometry();
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
                    g_dead_vec_table[pt][f][sq].in_board = static_cast<int8_t>(in_b);
                    g_dead_vec_table[pt][f][sq].out_board = static_cast<int8_t>(dtbl.count - in_b);
                }
            }
        }

        g_eval_params = EvalParams::defaults();
        const char* env_path = std::getenv("HOMEI_PARAMS");
        if (env_path && env_path[0] != '\0') {
            std::ifstream ifs(env_path);
            if (ifs.is_open()) {
                parse_params_stream(ifs, g_eval_params);
            }
        }
        g_eval_feats = EngineFeatures::get();
        g_eval_fast_eligible = g_eval_feats.koma_eval && g_eval_feats.siege_eval && g_eval_feats.tapered_v35_eval;
        rebuild_eval_sq_table();
    });
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
inline bool has_siege_ray(const Position& pos, int sq, const Piece& slider, int enemy_king_sq) {
    if (enemy_king_sq < 0 || enemy_king_sq >= NUM_SQUARES) return false;
    const Bitboard& kz = BB_KING_ZONE[enemy_king_sq];
    if (slider.type == LONG && (BB_ORTHO_MASK[sq] & kz).empty()) return false;
    if (slider.type == PHUONG && (BB_DIAG_MASK[sq] & kz).empty()) return false;
    if (slider.type == XA && (BB_RAY_ORTHO[sq][slider.facing & 3] & kz).empty()) return false;
    Bitboard reach = piece_attacks_bb(pos, sq, slider.type, slider.facing) & (~pos.bb_occ | Bitboard::from_sq(enemy_king_sq));
    return (reach & kz).any();
}

void eval_side_breakdown(
    const Position& pos,
    Side s,
    const Bitboard bb_att[2],
    bool koma_on,
    bool siege_on,
    bool v35_on,
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
    if (koma_on && !v35_on) {
        if (!(pos.reserve[s] & RESERVE_LONG)) ++developed_units;
        if (!(pos.reserve[s] & RESERVE_PHUONG)) ++developed_units;
    }

    int attack_units = 0;

    // 2. Board squares owned by `s`
    Bitboard side_bb = pos.bb_side[s];
    while (side_bb.any()) {
        int sq = side_bb.pop_lsb();
        uint8_t csize = pos.height[sq];
        const Piece top = pos.top_piece[sq];

        int r = sq_row(sq);
        int col = sq_col(sq);
        int rel_row = (s == CAM) ? r : (8 - r);

        // Material of all pieces in stack
        fixed_score += eval_piece_board_value(top.type);
        if (csize >= 2) {
            for (uint8_t idx = pos.buried_head[sq]; idx != BURIED_NONE; idx = pos.buried_next[idx]) {
                fixed_score += eval_piece_board_value(pos.buried_piece[idx].type);
            }
        }

        // Stack synergy & Step 1b (Buried slider and battery)
        if (csize >= 2) {
            feat[TERM_STACK_BASE] += 1;
            const Piece second = pos.cell_second(sq);
            bool second_is_slider = (second.type == LONG || second.type == PHUONG || second.type == XA);
            if (!koma_on) {
                if (second_is_slider && slider_aims_at_ek(r, col, second, ekr, ekc)) {
                    feat[TERM_STACK_BATTERY] += 1;
                }
            } else if (second_is_slider) {
                bool siege_ray = has_siege_ray(pos, sq, second, enemy_king_sq);
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
                    if ((!v35_on && slider_aims_at_ek(r, col, second, ekr, ekc)) || siege_ray) {
                        feat[TERM_STACK_BATTERY] += 1;
                    }
                    if (siege_ray) {
                        feat[TERM_KOMA_MOBILE_BATTERY] += 1;
                    }
                }
            }
        }

        // Orientation & activity of top piece (Section 4.3 item 6 + Step 1a, 1c, 1d + Stage 3)
        if (top.type != VUONG) {
            int center_dist = std::abs(col - 4);
            bool sq_enemy_att = bb_att[enemy].test_bit(sq);

            if (koma_on) {
                if (top.type != BINH) {
                    if (rel_row >= 1) ++developed_units;
                    else if (top.type == KI || top.type == SI) ++backrank_ki_si_count;
                } else if (rel_row >= 3 || csize >= 2) {
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

            if (siege_on) {
                // Stage 3.2: Siege units targeting enemy King 3x3 zone
                if (ekr >= 0) {
                    int manh_ek = std::abs(r - ekr) + std::abs(col - ekc);
                    if (manh_ek <= 3) {
                        bool faces_ek = (top.type == LONG || top.type == PHUONG) ||
                                        (top.facing == 0 && ekr < r) ||
                                        (top.facing == 2 && ekr > r) ||
                                        (top.facing == 1 && ekc > col) ||
                                        (top.facing == 3 && ekc < col);
                        if (faces_ek) {
                            attack_units += 2;
                            if (csize >= 2) attack_units += 2;
                        }
                    } else {
                        bool ray_hit = false;
                        if (top.type == LONG || top.type == PHUONG || top.type == XA) {
                            ray_hit = has_siege_ray(pos, sq, top, enemy_king_sq);
                        }
                        if (!ray_hit && csize >= 2) {
                            const Piece sec = pos.cell_second(sq);
                            if (sec.type == LONG || sec.type == PHUONG || sec.type == XA) {
                                ray_hit = has_siege_ray(pos, sq, sec, enemy_king_sq);
                            }
                        }
                        if (ray_hit) {
                            attack_units += 2;
                        }
                    }
                }

                // Stage 3.4: Vanguard support for SI, KI, XA at rel_row >= 3
                if ((top.type == SI || top.type == KI || top.type == XA) && rel_row >= 3) {
                    bool supported = bb_att[s].test_bit(sq);
                    if (!supported) {
                        int r_front = r + (s == CAM ? 1 : -1);
                        if (in_bounds(r_front, col)) {
                            int fsq = make_sq(r_front, col);
                            if (pos.height[fsq] > 0 && pos.cell_owner(fsq) == s && pos.top_piece[fsq].type == BINH) {
                                supported = true;
                            }
                        }
                    }
                    if (supported) {
                        feat[TERM_SIEGE_VANGUARD_SUPPORT] += 1;
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
                        int r_behind = r + (s == CAM ? -1 : 1);
                        bool guards_long_or_king = false;
                        if (in_bounds(r_behind, c_adj)) {
                            int bsq = make_sq(r_behind, c_adj);
                            if (pos.height[bsq] > 0 && pos.cell_owner(bsq) == s &&
                                (pos.top_piece[bsq].type == LONG || pos.top_piece[bsq].type == VUONG)) {
                                guards_long_or_king = true;
                            }
                        }
                        if (pos.height[adj_sq] > 0 && pos.cell_owner(adj_sq) == s && guards_long_or_king) {
                            feat[TERM_BINH_GUARD_LONG_KING] += 1;
                        }
                    }
                } else if (top.facing == backward_dir) {
                    feat[TERM_BINH_FORWARD] -= 1;
                }
                if (okr >= 0 && std::abs(r - okr) <= 1 && std::abs(col - okc) <= 1 &&
                    (!v35_on || v35::is_king_sheltered(s, okr, okc))) {
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
                const Bitboard& step_bb = BB_STEP_ATTACKS[top.type][top.facing][sq];
                feat[TERM_STEP_ATTACK_ENEMY] += (step_bb & pos.bb_side[enemy]).popcount();
                if (top.facing == forward_dir) {
                    fixed_score += 6 * (step_bb & pos.bb_side[s] & pos.bb_type[BINH]).popcount();
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
                feat[TERM_STEP_ATTACK_ENEMY] += (BB_STEP_ATTACKS[KI][top.facing][sq] & pos.bb_side[enemy]).popcount();
            } else if (top.type == LONG || top.type == PHUONG) {
                feat[TERM_CENTER_COL_WEIGHT] += (4 - center_dist);
                // Stage 3.1: Fix LONG/PHUONG losing active rank bonus when entering ranks 6, 7, 8
                if (siege_on ? (rel_row >= 3) : (rel_row >= 3 && rel_row <= 5)) {
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

    // Stage 3.2: Prisoner siege pressure & non-linear siege units
    if (siege_on) {
        if (enemy_king_sq < NUM_SQUARES) {
            int unctrl_empty_ek = (BB_KING_ZONE[enemy_king_sq] & ~(pos.bb_occ | bb_att[enemy])).popcount();
            if (unctrl_empty_ek >= 2) {
                int att_prisoners = pos.prisoners[s][KI] + pos.prisoners[s][TUONG] +
                                    pos.prisoners[s][SI] + pos.prisoners[s][XA] +
                                    pos.prisoners[s][LONG] + pos.prisoners[s][PHUONG];
                attack_units += std::min(4, att_prisoners);
            }
        }
        feat[TERM_SIEGE_UNITS_LINEAR] += attack_units;
        feat[TERM_SIEGE_UNITS_QUAD] += std::min(64, attack_units * attack_units) / 4;
    }

    // 5. King Safety (Section 4.3 item 5) + Stage 3.3 (TERM_SIEGE_DROP_HOLE)
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
        int undefended_empty_king_adj = 0;
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if (dr == 0 && dc == 0) continue;
                int nr = okr + dr;
                int nc = okc + dc;
                if (!in_bounds(nr, nc)) continue;
                int nsq = make_sq(nr, nc);
                bool enemy_att = bb_att[enemy].test_bit(nsq);
                uint8_t nh = pos.height[nsq];
                if (nh > 0 && pos.cell_owner(nsq) == s) {
                    int nr_rel = (s == CAM) ? nr : (8 - nr);
                    if (nr_rel <= 1 && shield_count < 3 && (!v35_on || v35::is_king_sheltered(s, okr, okc))) {
                        fixed_score += 16;
                        ++shield_count;
                    }
                    if (enemy_att && !bb_att[s].test_bit(nsq)) {
                        fixed_score -= 10;
                    }
                } else {
                    if (nh == 0 && !bb_att[s].test_bit(nsq)) {
                        ++undefended_empty_king_adj;
                    }
                    if (enemy_att) {
                        fixed_score -= 20;
                        if (enemy_prisoners >= 2) {
                            fixed_score -= 25 * eff_prisoners;
                        } else if (nh == 0 && enemy_prisoners >= 1) {
                            fixed_score -= 18;
                        }
                    } else if (nh == 0 && enemy_prisoners >= 2) {
                        fixed_score -= 5 * eff_prisoners;
                    }
                }
            }
        }
        if (siege_on && eff_prisoners > 0 && undefended_empty_king_adj > 0) {
            feat[TERM_SIEGE_DROP_HOLE] -= undefended_empty_king_adj * eff_prisoners;
        }
    }
}

} // namespace

void init_eval_tables() {
    ensure_eval_params_loaded();
}

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
        rebuild_eval_sq_table();
        return true;
    }
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;
    EvalParams tmp = EvalParams::defaults();
    parse_params_stream(ifs, tmp);
    g_eval_params = tmp;
    rebuild_eval_sq_table();
    return true;
}

static EvalBreakdown evaluate_breakdown_impl(
    const Position& pos,
    int alpha,
    int beta,
    bool allow_lazy,
    bool& out_is_lazy,
    v35::V35Trace* out_trace = nullptr,
    int* out_v35_delta = nullptr
) {
#ifdef HOMEI_PROFILE
    SearchProfile& prof = get_search_profile();
    const bool prof_on = prof.enabled;
    std::chrono::steady_clock::time_point t_ev0;
    if (prof_on) {
        ++prof.eval_calls;
        t_ev0 = std::chrono::steady_clock::now();
    }
#endif

    ensure_eval_params_loaded();
    const EvalParams& params = g_eval_params;
    const EngineFeatures& feats = EngineFeatures::get();
    bool koma_on = feats.koma_eval;
    bool siege_on = feats.siege_eval;
    bool v35_on = feats.tapered_v35_eval || (out_trace != nullptr);

    Side stm = static_cast<Side>(pos.side_to_move);
    Side ntm = other_side(stm);

    Bitboard bb_att[2]{};
    int long_mob[2]{};
    int phuong_mob[2]{};
    int kd_units[2]{};
    int occ_sqs[44]{};
    int occ_count = 0;
    int fixed_stm = 0, fixed_ntm = 0;
    int feat_stm[EVAL_TERM_COUNT]{};
    int feat_ntm[EVAL_TERM_COUNT]{};

    Bitboard ek_zones[2]{};
    if (pos.king_sq[CAM] < NUM_SQUARES) ek_zones[CAM] = BB_KING_ZONE[pos.king_sq[CAM]];
    if (pos.king_sq[XANH] < NUM_SQUARES) ek_zones[XANH] = BB_KING_ZONE[pos.king_sq[XANH]];
    constexpr int KD_WEIGHT[8] = {0, 3, 2, 3, 2, 5, 4, 1};

    Bitboard occ = pos.bb_occ;
    while (occ.any()) {
        int sq = occ.pop_lsb();
        if (occ_count < 44) occ_sqs[occ_count++] = sq;
        uint8_t csize = pos.height[sq];
        Side owner = pos.cell_owner(sq);
        const Piece top = pos.top_piece[sq];
        uint8_t top_t = top.type;
        Bitboard att = piece_attacks_bb(pos, sq, top_t, top.facing);
        bb_att[owner] |= att;

        if (koma_on && (top_t == LONG || top_t == PHUONG)) {
            Bitboard empty_or_enemy = att & ~pos.bb_side[owner];
            int mob_base = empty_or_enemy.popcount();
            int mob_prime = (empty_or_enemy & BB_PRIME_ZONE[owner]).popcount();
            if (top_t == LONG) long_mob[owner] += mob_base;
            else phuong_mob[owner] += mob_base;
            int* f_arr = (owner == stm) ? feat_stm : feat_ntm;
            f_arr[TERM_KOMA_SLIDER_MOB_BASE] += std::min(12, mob_base);
            f_arr[TERM_KOMA_SLIDER_MOB_PRIME] += std::min(8, mob_prime);
        }

        if (v35_on && top_t != VUONG) {
            Side enemy = other_side(owner);
            if ((att & ek_zones[enemy]).any()) {
                kd_units[enemy] += KD_WEIGHT[top_t] + (csize >= 2 ? 1 : 0);
            }
        }
    }

    eval_side_breakdown(pos, stm, bb_att, koma_on, siege_on, v35_on, fixed_stm, feat_stm);
    eval_side_breakdown(pos, ntm, bb_att, koma_on, siege_on, v35_on, fixed_ntm, feat_ntm);

    int v35_delta = v35_on ? v35::evaluate_v35_tapered(pos, bb_att, long_mob, phuong_mob, kd_units, occ_sqs, occ_count, out_trace) : 0;
    if (out_v35_delta) *out_v35_delta = v35_delta;

    EvalBreakdown out;
    out.base_fixed = (fixed_stm - fixed_ntm) + v35_delta;
    out.total = out.base_fixed;
    for (int i = 0; i < EVAL_TERM_COUNT; ++i) {
        out.raw[i] = feat_stm[i] - feat_ntm[i];
        out.weight[i] = params.w[i];
        out.contrib[i] = out.raw[i] * out.weight[i];
        out.total += out.contrib[i];
    }

    Bitboard hanging_cands = (pos.bb_side[CAM] & bb_att[XANH]) | (pos.bb_side[XANH] & bb_att[CAM]);

    out_is_lazy = false;
    if (allow_lazy && feats.lazy_eval && (out.total < alpha - 300 || out.total > beta + 300)) {
        // Compute O(1) upper/lower bound on hanging_score using cell_material_value on attacked squares
        int ub_ntm1 = 0, ub_ntm2 = 0;
        int ub_stm1 = 0, ub_stm2 = 0;
        Bitboard hb = hanging_cands;
        while (hb.any()) {
            int sq = hb.pop_lsb();
            if (pos.cell_contains_king(sq)) continue;
            Side owner = pos.cell_owner(sq);
            int mv = cell_material_value(pos, sq);
            if (owner == ntm) {
                if (mv >= ub_ntm1) { ub_ntm2 = ub_ntm1; ub_ntm1 = mv; }
                else if (mv > ub_ntm2) { ub_ntm2 = mv; }
            } else {
                if (mv >= ub_stm1) { ub_stm2 = ub_stm1; ub_stm1 = mv; }
                else if (mv > ub_stm2) { ub_stm2 = mv; }
            }
        }
        int max_pos_hang = (ub_ntm1 * 3) / 4 + (ub_ntm2 / 4);
        int max_neg_hang = -((ub_stm1 / 4) + (ub_stm2 * 3) / 4);
        if ((out.total < alpha - 300 && out.total + max_pos_hang < alpha - 300) ||
            (out.total > beta + 300 && out.total + max_neg_hang > beta + 300)) {
            out_is_lazy = true;
#ifdef HOMEI_PROFILE
            if (prof_on) {
                ++prof.eval_lazy_skips;
                auto t_ev1 = std::chrono::steady_clock::now();
                prof.eval_total_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t_ev1 - t_ev0).count()
                );
            }
#endif
            return out;
        }
    }

    int ntm_max1 = 0, ntm_max2 = 0;
    int stm_max1 = 0, stm_max2 = 0;
    Bitboard hb = hanging_cands;
    while (hb.any()) {
        int sq = hb.pop_lsb();
        if (pos.cell_contains_king(sq)) continue;
        Side owner = pos.cell_owner(sq);

        int loss = 0;
#ifdef HOMEI_PROFILE
        if (prof_on) {
            ++prof.hanging_calls;
            auto th0 = std::chrono::steady_clock::now();
            loss = square_hanging_loss_fast(pos, sq, bb_att);
            auto th1 = std::chrono::steady_clock::now();
            prof.hanging_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(th1 - th0).count()
            );
        } else {
            loss = square_hanging_loss_fast(pos, sq, bb_att);
        }
#else
        loss = square_hanging_loss_fast(pos, sq, bb_att);
#endif
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
    out.base_fixed += hanging_score;
    out.total += hanging_score;

#ifdef HOMEI_PROFILE
    if (prof_on) {
        auto t_ev1 = std::chrono::steady_clock::now();
        prof.eval_total_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t_ev1 - t_ev0).count()
        );
    }
#endif
    return out;
}

// Ultra-fast single-pass fused evaluation (100% score-equivalent to evaluate_breakdown when koma+siege+v35 enabled).
static int evaluate_fast(const Position& pos, int alpha, int beta, bool allow_lazy, bool& out_is_lazy) {
#ifdef HOMEI_PROFILE
    SearchProfile& prof = get_search_profile();
    const bool prof_on = prof.enabled;
    std::chrono::steady_clock::time_point t_ev0;
    if (prof_on) {
        ++prof.eval_calls;
        t_ev0 = std::chrono::steady_clock::now();
    }
#endif

    const int* w = g_eval_params.w;
    const Side stm = static_cast<Side>(pos.side_to_move);
    const Side ntm = other_side(stm);

    int score = 0;
    int mg_sum = v35::V35_STRUCT_MG[v35::S_TEMPO];
    int eg_sum = v35::V35_STRUCT_EG[v35::S_TEMPO];
    int phase_raw = 0;

    int threat_lp[2]{};
    int minors[2]{};
    int phuong_total[2]{};
    int lp_on_board[2]{};
    int long_mob[2]{};
    int phuong_mob[2]{};
    int kd_units[2]{};
    int attack_units[2]{};
    int developed_units[2]{};
    int backrank_ki_si[2]{};
    int total_prisoners[2]{};
    uint16_t binh_row_mask[2][9]{};

    // 1. Hand: Reserve & Prisoners for both sides
    int mat_diff = 0;
    for (int si = 0; si < 2; ++si) {
        const Side s = static_cast<Side>(si);
        const int sign = (s == stm) ? 1 : -1;
        if (pos.reserve[s] & RESERVE_LONG) {
            int rv = eval_piece_reserve_value(LONG);
            score += sign * rv;
            mat_diff += sign * rv;
            mg_sum += sign * g_mat_mg[LONG];
            eg_sum += sign * g_mat_eg[LONG];
            phase_raw += 4;
            ++threat_lp[si];
        }
        if (pos.reserve[s] & RESERVE_PHUONG) {
            int rv = eval_piece_reserve_value(PHUONG);
            score += sign * rv;
            mat_diff += sign * rv;
            mg_sum += sign * g_mat_mg[PHUONG];
            eg_sum += sign * g_mat_eg[PHUONG];
            phase_raw += 4;
            ++threat_lp[si];
            ++phuong_total[si];
        }
        int psum = 0;
        for (int pt = 1; pt < 8; ++pt) {
            int cnt = pos.prisoners[s][pt];
            if (cnt > 0) {
                psum += cnt;
                int pv = cnt * eval_piece_prisoner_value(static_cast<uint8_t>(pt));
                score += sign * pv;
                mat_diff += sign * pv;
                mg_sum += sign * cnt * g_pris_mg[pt];
                eg_sum += sign * cnt * g_pris_eg[pt];
            }
        }
        total_prisoners[si] = psum;
    }

    // Stage-0 Ultra-Fast Early Lazy Eval (~25ns): skip full attack/mobility/king/hanging eval
    // when pure material difference alone is > 1100 cp outside [alpha, beta]!
    out_is_lazy = false;
    if (allow_lazy && g_eval_feats.lazy_eval && alpha > -SCORE_MATE + 1500 && beta < SCORE_MATE - 1500) {
        int m_est = mat_diff;
        const Bitboard s_bb = pos.bb_side[stm];
        const Bitboard n_bb = pos.bb_side[ntm];
        m_est += ((s_bb & pos.bb_type[BINH]).popcount()   - (n_bb & pos.bb_type[BINH]).popcount())   * eval_piece_board_value(BINH);
        m_est += ((s_bb & pos.bb_type[XA]).popcount()     - (n_bb & pos.bb_type[XA]).popcount())     * eval_piece_board_value(XA);
        m_est += ((s_bb & pos.bb_type[SI]).popcount()     - (n_bb & pos.bb_type[SI]).popcount())     * eval_piece_board_value(SI);
        m_est += ((s_bb & pos.bb_type[KI]).popcount()     - (n_bb & pos.bb_type[KI]).popcount())     * eval_piece_board_value(KI);
        m_est += ((s_bb & pos.bb_type[TUONG]).popcount()  - (n_bb & pos.bb_type[TUONG]).popcount())  * eval_piece_board_value(TUONG);
        m_est += ((s_bb & pos.bb_type[PHUONG]).popcount() - (n_bb & pos.bb_type[PHUONG]).popcount()) * eval_piece_board_value(PHUONG);
        m_est += ((s_bb & pos.bb_type[LONG]).popcount()   - (n_bb & pos.bb_type[LONG]).popcount())   * eval_piece_board_value(LONG);
        Bitboard st_m = pos.bb_stack;
        while (st_m.any()) {
            int sq = st_m.pop_lsb();
            int sgn = (pos.cell_owner(sq) == stm) ? 1 : -1;
            for (uint8_t idx = pos.buried_head[sq]; idx != BURIED_NONE; idx = pos.buried_next[idx]) {
                m_est += sgn * eval_piece_board_value(pos.buried_piece[idx].type);
            }
        }
        if (m_est < alpha - 1100 || m_est > beta + 1100) {
            out_is_lazy = true;
#ifdef HOMEI_PROFILE
            if (prof_on) {
                ++prof.eval_lazy_skips;
                auto t_ev1 = std::chrono::steady_clock::now();
                prof.eval_total_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t_ev1 - t_ev0).count()
                );
            }
#endif
            return m_est;
        }
    }

    const int ksq[2] = {pos.king_sq[CAM], pos.king_sq[XANH]};
    const int kr[2] = {
        (ksq[0] < NUM_SQUARES) ? sq_row(ksq[0]) : -1,
        (ksq[1] < NUM_SQUARES) ? sq_row(ksq[1]) : -1
    };
    const int kc[2] = {
        (ksq[0] < NUM_SQUARES) ? sq_col(ksq[0]) : -1,
        (ksq[1] < NUM_SQUARES) ? sq_col(ksq[1]) : -1
    };
    const bool k_sheltered[2] = {
        v35::is_king_sheltered(CAM, kr[0], kc[0]),
        v35::is_king_sheltered(XANH, kr[1], kc[1])
    };
    const Bitboard k_zones[2] = {
        (ksq[0] < NUM_SQUARES) ? BB_KING_ZONE[ksq[0]] : Bitboard{},
        (ksq[1] < NUM_SQUARES) ? BB_KING_ZONE[ksq[1]] : Bitboard{}
    };
    const Bitboard k_bb[2] = {
        (ksq[0] < NUM_SQUARES) ? Bitboard::from_sq(ksq[0]) : Bitboard{},
        (ksq[1] < NUM_SQUARES) ? Bitboard::from_sq(ksq[1]) : Bitboard{}
    };
    const Bitboard empty_bb = ~pos.bb_occ;

    Bitboard bb_att[2]{};
    Bitboard bb_att2[2]{};
    Bitboard bb_att_stack[2]{};

    constexpr int PHASE_W[8] = {0, 3, 2, 3, 2, 4, 4, 1};
    constexpr int dr4[4] = {-1, 0, 1, 0};
    constexpr int dc4[4] = {0, 1, 0, -1};

    // 2. Specialized per-piece-type bitboard loops (zero dispatch branches, zero csize checks!)

    // 2a. BINH (~half of all pieces on the board!)
    {
        Bitboard bb = pos.bb_type[BINH];
        while (bb.any()) {
            const int sq = bb.pop_lsb();
            const Piece top = pos.top_piece[sq];
            const Side s = static_cast<Side>(top.owner);
            const Side enemy = other_side(s);
            const int sign = (s == stm) ? 1 : -1;
            const uint8_t top_f = top.facing;

            const EvalSqEntry& e = g_sq_table[s][BINH][top_f][sq];
            score += sign * static_cast<int>(e.base);
            mg_sum += sign * static_cast<int>(e.mg);
            eg_sum += sign * static_cast<int>(e.eg);
            phase_raw += 1;

            const Bitboard& att = BB_STEP_ATTACKS[BINH][top_f][sq];
            bb_att2[s] |= (bb_att[s] & att);
            bb_att[s] |= att;
            if ((att & k_zones[enemy]).any()) kd_units[enemy] += 1;

            const int r = sq_row(sq);
            const int col = sq_col(sq);
            const int rel_r = (s == CAM) ? r : (8 - r);
            binh_row_mask[s][r] |= static_cast<uint16_t>(1u << col);
            if (rel_r >= 3) ++developed_units[s];

            const int ekr = kr[enemy];
            const int ekc = kc[enemy];
            if (ekr >= 0 && (std::abs(r - ekr) + std::abs(col - ekc)) <= 3) {
                if ((top_f == 0 && ekr < r) || (top_f == 2 && ekr > r) ||
                    (top_f == 1 && ekc > col) || (top_f == 3 && ekc < col)) {
                    attack_units[s] += 2;
                }
            }

            if (top_f == 1 || top_f == 3) {
                int c_adj = col + (top_f == 1 ? 1 : -1);
                if (in_bounds(r, c_adj)) {
                    int adj_sq = make_sq(r, c_adj);
                    if (pos.height[adj_sq] > 0 && pos.cell_owner(adj_sq) == s) {
                        int r_behind = r + (s == CAM ? -1 : 1);
                        if (in_bounds(r_behind, c_adj)) {
                            int bsq = make_sq(r_behind, c_adj);
                            if (pos.height[bsq] > 0 && pos.cell_owner(bsq) == s &&
                                (pos.top_piece[bsq].type == LONG || pos.top_piece[bsq].type == VUONG)) {
                                score += sign * w[TERM_BINH_GUARD_LONG_KING];
                            }
                        }
                    }
                }
            }
            if (k_sheltered[s] && k_zones[s].test_bit(sq)) {
                if (col == kc[s] && r == kr[s] + (s == CAM ? 1 : -1)) {
                    score += sign * w[TERM_BINH_KING_SHIELD_FRONT];
                } else {
                    score += sign * w[TERM_BINH_KING_SHIELD_SIDE];
                }
                if (sq != ksq[s] && !v35::is_edge_dead_facing(r, col, top_f)) {
                    mg_sum += sign * v35::V35_STRUCT_MG[v35::S_CASTLE_B_SHIELD];
                    eg_sum += sign * v35::V35_STRUCT_EG[v35::S_CASTLE_B_SHIELD];
                }
            }
        }
    }

    // 2b. TUONG and SI
    {
        Bitboard bb = pos.bb_type[TUONG] | pos.bb_type[SI];
        while (bb.any()) {
            const int sq = bb.pop_lsb();
            const Piece top = pos.top_piece[sq];
            const Side s = static_cast<Side>(top.owner);
            const Side enemy = other_side(s);
            const int sign = (s == stm) ? 1 : -1;
            const uint8_t top_t = top.type;
            const uint8_t top_f = top.facing;

            const EvalSqEntry& e = g_sq_table[s][top_t][top_f][sq];
            score += sign * static_cast<int>(e.base);
            mg_sum += sign * static_cast<int>(e.mg);
            eg_sum += sign * static_cast<int>(e.eg);
            const int pw = (top_t == TUONG) ? 3 : 2;
            phase_raw += pw;
            ++minors[s];

            const Bitboard& att = BB_STEP_ATTACKS[top_t][top_f][sq];
            bb_att2[s] |= (bb_att[s] & att);
            bb_att[s] |= att;
            if ((att & k_zones[enemy]).any()) kd_units[enemy] += pw;

            const int r = sq_row(sq);
            const int col = sq_col(sq);
            const int rel_r = (s == CAM) ? r : (8 - r);
            if (rel_r >= 1) ++developed_units[s];
            else if (top_t == SI) ++backrank_ki_si[s];

            score += sign * (att & pos.bb_side[enemy]).popcount() * w[TERM_STEP_ATTACK_ENEMY];
            const uint8_t fwd = initial_facing(s);
            const uint8_t bwd = initial_facing(enemy);
            if (top_f == fwd) {
                score += sign * 6 * (att & pos.bb_side[s] & pos.bb_type[BINH]).popcount();
            }

            const int ekr = kr[enemy];
            const int ekc = kc[enemy];
            if (ekr >= 0 && (std::abs(r - ekr) + std::abs(col - ekc)) <= 3) {
                const bool secondary_ek =
                    (top_f == 0 && ekr < r) || (top_f == 2 && ekr > r) ||
                    (top_f == 1 && ekc > col) || (top_f == 3 && ekc < col);
                if (secondary_ek) {
                    attack_units[s] += 2;
                    int dr_ek = std::abs(r - ekr);
                    int dc_ek = std::abs(col - ekc);
                    bool primary_ek = (dr_ek >= dc_ek)
                        ? ((top_f == 0 && ekr < r) || (top_f == 2 && ekr > r))
                        : ((top_f == 1 && ekc > col) || (top_f == 3 && ekc < col));
                    score += sign * (primary_ek ? 14 : 4);
                }
            } else {
                if (top_f == fwd) score += sign * 6;
                else if (top_f == bwd) score -= sign * 6;
            }

            const int okr = kr[s];
            if (okr >= 0 && sq != ksq[s]) {
                int d_king = std::max(std::abs(r - okr), std::abs(col - kc[s]));
                if (d_king <= 2) {
                    int ok_rel = (s == CAM) ? okr : (8 - okr);
                    if (ok_rel <= 1) {
                        mg_sum += sign * v35::V35_STRUCT_MG[v35::S_CASTLE_WALL_TS];
                        eg_sum += sign * v35::V35_STRUCT_EG[v35::S_CASTLE_WALL_TS];
                    }
                    if (d_king <= 1) {
                        mg_sum += sign * v35::V35_STRUCT_MG[v35::S_KING_RING_DEFENDERS];
                        eg_sum += sign * v35::V35_STRUCT_EG[v35::S_KING_RING_DEFENDERS];
                    }
                }
            }
        }
    }

    // 2c. KI
    {
        Bitboard bb = pos.bb_type[KI];
        while (bb.any()) {
            const int sq = bb.pop_lsb();
            const Piece top = pos.top_piece[sq];
            const Side s = static_cast<Side>(top.owner);
            const Side enemy = other_side(s);
            const int sign = (s == stm) ? 1 : -1;
            const uint8_t top_f = top.facing;

            const EvalSqEntry& e = g_sq_table[s][KI][top_f][sq];
            score += sign * static_cast<int>(e.base);
            mg_sum += sign * static_cast<int>(e.mg);
            eg_sum += sign * static_cast<int>(e.eg);
            phase_raw += 3;
            ++minors[s];

            const Bitboard& att = BB_STEP_ATTACKS[KI][top_f][sq];
            bb_att2[s] |= (bb_att[s] & att);
            bb_att[s] |= att;
            if ((att & k_zones[enemy]).any()) kd_units[enemy] += 3;

            const int r = sq_row(sq);
            const int col = sq_col(sq);
            const int rel_r = (s == CAM) ? r : (8 - r);
            if (rel_r >= 1) ++developed_units[s];
            else ++backrank_ki_si[s];

            score += sign * (att & pos.bb_side[enemy]).popcount() * w[TERM_STEP_ATTACK_ENEMY];

            const int ekr = kr[enemy];
            const int ekc = kc[enemy];
            if (ekr >= 0 && (std::abs(r - ekr) + std::abs(col - ekc)) <= 3) {
                if ((top_f == 0 && ekr < r) || (top_f == 2 && ekr > r) ||
                    (top_f == 1 && ekc > col) || (top_f == 3 && ekc < col)) {
                    attack_units[s] += 2;
                }
            }
            if (k_zones[s].test_bit(sq) && sq != ksq[s]) {
                mg_sum += sign * v35::V35_STRUCT_MG[v35::S_KING_RING_DEFENDERS];
                eg_sum += sign * v35::V35_STRUCT_EG[v35::S_KING_RING_DEFENDERS];
            }
        }
    }

    // 2d. XA
    {
        Bitboard bb = pos.bb_type[XA];
        while (bb.any()) {
            const int sq = bb.pop_lsb();
            const Piece top = pos.top_piece[sq];
            const Side s = static_cast<Side>(top.owner);
            const Side enemy = other_side(s);
            const int sign = (s == stm) ? 1 : -1;
            const uint8_t top_f = top.facing;

            const EvalSqEntry& e = g_sq_table[s][XA][top_f][sq];
            score += sign * static_cast<int>(e.base);
            mg_sum += sign * static_cast<int>(e.mg);
            eg_sum += sign * static_cast<int>(e.eg);
            phase_raw += 2;
            ++minors[s];

            const Bitboard att = ray_ortho_attack(sq, top_f, pos.bb_occ);
            bb_att2[s] |= (bb_att[s] & att);
            bb_att[s] |= att;
            if ((att & k_zones[enemy]).any()) kd_units[enemy] += 2;

            const int r = sq_row(sq);
            const int col = sq_col(sq);
            if (((s == CAM) ? r : (8 - r)) >= 1) ++developed_units[s];

            const int ekr = kr[enemy];
            const int ekc = kc[enemy];
            if (ekr >= 0) {
                if ((std::abs(r - ekr) + std::abs(col - ekc)) <= 3) {
                    if ((top_f == 0 && ekr < r) || (top_f == 2 && ekr > r) ||
                        (top_f == 1 && ekc > col) || (top_f == 3 && ekc < col)) {
                        attack_units[s] += 2;
                    }
                } else if (((att & (empty_bb | k_bb[enemy])) & k_zones[enemy]).any()) {
                    attack_units[s] += 2;
                }
            }

            int nr = r + dr4[top_f];
            int nc = col + dc4[top_f];
            if (in_bounds(nr, nc)) {
                int asq = make_sq(nr, nc);
                if (pos.height[asq] == 0 || pos.cell_owner(asq) != s) {
                    mg_sum += sign * v35::V35_STRUCT_MG[v35::S_XA_OPEN_LANE];
                    eg_sum += sign * v35::V35_STRUCT_EG[v35::S_XA_OPEN_LANE];
                }
            }
        }
    }

    // 2e. LONG
    {
        Bitboard bb = pos.bb_type[LONG];
        while (bb.any()) {
            const int sq = bb.pop_lsb();
            const Piece top = pos.top_piece[sq];
            const Side s = static_cast<Side>(top.owner);
            const Side enemy = other_side(s);
            const int sign = (s == stm) ? 1 : -1;

            const EvalSqEntry& e = g_sq_table[s][LONG][top.facing][sq];
            score += sign * static_cast<int>(e.base);
            mg_sum += sign * static_cast<int>(e.mg);
            eg_sum += sign * static_cast<int>(e.eg);
            phase_raw += 4;
            ++threat_lp[s];
            ++lp_on_board[s];

            const Bitboard att = ortho_attacks(sq, pos.bb_occ);
            bb_att2[s] |= (bb_att[s] & att);
            bb_att[s] |= att;
            if ((att & k_zones[enemy]).any()) kd_units[enemy] += 5;

            Bitboard empty_or_enemy = att & ~pos.bb_side[s];
            int mob_base = empty_or_enemy.popcount();
            int mob_prime = (empty_or_enemy & BB_PRIME_ZONE[s]).popcount();
            long_mob[s] += mob_base;
            score += sign * (std::min(12, mob_base) * w[TERM_KOMA_SLIDER_MOB_BASE] +
                             std::min(8, mob_prime) * w[TERM_KOMA_SLIDER_MOB_PRIME]);

            const int r = sq_row(sq);
            const int col = sq_col(sq);
            if (((s == CAM) ? r : (8 - r)) >= 1) ++developed_units[s];

            const int ekr = kr[enemy];
            if (ekr >= 0) {
                if ((std::abs(r - ekr) + std::abs(col - kc[enemy])) <= 3 ||
                    ((att & (empty_bb | k_bb[enemy])) & k_zones[enemy]).any()) {
                    attack_units[s] += 2;
                }
            }
        }
    }

    // 2f. PHUONG
    {
        Bitboard bb = pos.bb_type[PHUONG];
        while (bb.any()) {
            const int sq = bb.pop_lsb();
            const Piece top = pos.top_piece[sq];
            const Side s = static_cast<Side>(top.owner);
            const Side enemy = other_side(s);
            const int sign = (s == stm) ? 1 : -1;

            const EvalSqEntry& e = g_sq_table[s][PHUONG][top.facing][sq];
            score += sign * static_cast<int>(e.base);
            mg_sum += sign * static_cast<int>(e.mg);
            eg_sum += sign * static_cast<int>(e.eg);
            phase_raw += 4;
            ++threat_lp[s];
            ++phuong_total[s];
            ++lp_on_board[s];

            const Bitboard att = diag_attacks(sq, pos.bb_occ);
            bb_att2[s] |= (bb_att[s] & att);
            bb_att[s] |= att;
            if ((att & k_zones[enemy]).any()) kd_units[enemy] += 4;

            Bitboard empty_or_enemy = att & ~pos.bb_side[s];
            int mob_base = empty_or_enemy.popcount();
            int mob_prime = (empty_or_enemy & BB_PRIME_ZONE[s]).popcount();
            phuong_mob[s] += mob_base;
            score += sign * (std::min(12, mob_base) * w[TERM_KOMA_SLIDER_MOB_BASE] +
                             std::min(8, mob_prime) * w[TERM_KOMA_SLIDER_MOB_PRIME]);

            const int r = sq_row(sq);
            const int col = sq_col(sq);
            if (((s == CAM) ? r : (8 - r)) >= 1) ++developed_units[s];

            const int ekr = kr[enemy];
            if (ekr >= 0) {
                if ((std::abs(r - ekr) + std::abs(col - kc[enemy])) <= 3 ||
                    ((att & (empty_bb | k_bb[enemy])) & k_zones[enemy]).any()) {
                    attack_units[s] += 2;
                }
            }
        }
    }

    // 2g. VUONG (2 squares)
    {
        Bitboard bb = pos.bb_type[VUONG];
        while (bb.any()) {
            const int sq = bb.pop_lsb();
            const Piece top = pos.top_piece[sq];
            const Side s = static_cast<Side>(top.owner);
            const int sign = (s == stm) ? 1 : -1;
            const EvalSqEntry& e = g_sq_table[s][VUONG][top.facing][sq];
            score += sign * static_cast<int>(e.base);
            mg_sum += sign * static_cast<int>(e.mg);
            eg_sum += sign * static_cast<int>(e.eg);
            const Bitboard& att = BB_STEP_ATTACKS[VUONG][0][sq];
            bb_att2[s] |= (bb_att[s] & att);
            bb_att[s] |= att;
        }
    }

    // 2h. Stacks (csize >= 2; typically 0-3 squares total!)
    {
        Bitboard st_bb = pos.bb_stack;
        while (st_bb.any()) {
            const int sq = st_bb.pop_lsb();
            const Piece top = pos.top_piece[sq];
            const Side s = static_cast<Side>(top.owner);
            const Side enemy = other_side(s);
            const int sign = (s == stm) ? 1 : -1;
            const uint8_t top_t = top.type;
            const uint8_t top_f = top.facing;
            const int r = sq_row(sq);
            const int col = sq_col(sq);
            const int rel_r = (s == CAM) ? r : (8 - r);

            const Bitboard top_att = piece_attacks_bb(pos, sq, top_t, top_f);
            bb_att_stack[s] |= top_att;

            score += sign * w[TERM_STACK_BASE];
            bool has_long = (top_t == LONG);
            bool has_phuong = (top_t == PHUONG);
            bool b_ki = false, b_sl = false, b_ts = false;
            const Piece second = pos.buried_piece[pos.buried_head[sq]];
            for (uint8_t idx = pos.buried_head[sq]; idx != BURIED_NONE; idx = pos.buried_next[idx]) {
                const uint8_t bt = pos.buried_piece[idx].type;
                score += sign * eval_piece_board_value(bt);
                mg_sum += sign * g_mat_mg[bt];
                eg_sum += sign * g_mat_eg[bt];
                phase_raw += PHASE_W[bt];
                if (bt == LONG) { has_long = true; ++threat_lp[s]; b_sl = true; }
                else if (bt == PHUONG) { has_phuong = true; ++threat_lp[s]; ++phuong_total[s]; b_sl = true; }
                else if (bt == XA) { ++minors[s]; b_sl = true; }
                else if (bt == KI) { ++minors[s]; b_ki = true; }
                else if (bt == TUONG || bt == SI) { ++minors[s]; b_ts = true; }
            }

            if (top_t != LONG && has_long && !has_phuong) lp_on_board[s]++;
            if (top_t != PHUONG && has_phuong && !has_long) lp_on_board[s]++;

            bool sec_siege_ray = false;
            const bool second_is_slider = (second.type == LONG || second.type == PHUONG || second.type == XA);
            if (second_is_slider) {
                sec_siege_ray = has_siege_ray(pos, sq, second, ksq[enemy]);
                if (top_t == BINH || top_t == SI || top_t == TUONG) {
                    if (rel_r <= 3) score -= sign * w[TERM_KOMA_BURIED_HOME];
                    else if (!sec_siege_ray) score -= sign * w[TERM_KOMA_BURIED_NO_RAY];
                    else score += sign * w[TERM_STACK_BATTERY];
                } else if (sec_siege_ray) {
                    score += sign * (w[TERM_STACK_BATTERY] + w[TERM_KOMA_MOBILE_BATTERY]);
                }
            }

            if (has_long && has_phuong && rel_r <= 1) {
                mg_sum += sign * v35::V35_STRUCT_MG[v35::S_LP_HOME_STACK];
                eg_sum += sign * v35::V35_STRUCT_EG[v35::S_LP_HOME_STACK];
            }
            if (top_t == BINH && !v35::is_edge_dead_facing(r, col, top_f)) {
                if (b_ki && rel_r <= 6) {
                    mg_sum += sign * v35::V35_STRUCT_MG[v35::S_CARRIER_BK];
                    eg_sum += sign * v35::V35_STRUCT_EG[v35::S_CARRIER_BK];
                }
                if (b_sl && rel_r >= 2 && rel_r <= 7) {
                    mg_sum += sign * v35::V35_STRUCT_MG[v35::S_CARRIER_SLIDER];
                    eg_sum += sign * v35::V35_STRUCT_EG[v35::S_CARRIER_SLIDER];
                }
            }
            if (b_ts && rel_r <= 2 && (top_t == TUONG || top_t == SI || top_t == BINH)) {
                mg_sum += sign * v35::V35_STRUCT_MG[v35::S_STACK_DEFENSIVE];
                eg_sum += sign * v35::V35_STRUCT_EG[v35::S_STACK_DEFENSIVE];
            }

            if (top_t != VUONG) {
                if ((top_att & k_zones[enemy]).any()) kd_units[enemy] += 1;
                if (top_t == BINH && rel_r < 3) ++developed_units[s];
                const int ekr = kr[enemy];
                const int ekc = kc[enemy];
                if (ekr >= 0) {
                    if ((std::abs(r - ekr) + std::abs(col - ekc)) <= 3) {
                        const bool faces_ek = (top_t == LONG || top_t == PHUONG) ||
                                              (top_f == 0 && ekr < r) || (top_f == 2 && ekr > r) ||
                                              (top_f == 1 && ekc > col) || (top_f == 3 && ekc < col);
                        if (faces_ek) attack_units[s] += 2;
                    } else if (sec_siege_ray) {
                        bool top_ray_hit = (top_t == LONG || top_t == PHUONG || top_t == XA) &&
                                           ((top_att & (empty_bb | k_bb[enemy])) & k_zones[enemy]).any();
                        if (!top_ray_hit) attack_units[s] += 2;
                    }
                }
            }
        }
    }

    // 3. Bitboard-masked post-attack micro-passes (typically 0-3 bits each)
    const Bitboard non_bv = ~(pos.bb_type[BINH] | pos.bb_type[VUONG]);
    const Bitboard ts_mask = pos.bb_type[TUONG] | pos.bb_type[SI];
    const Bitboard skx_mask = pos.bb_type[SI] | pos.bb_type[KI] | pos.bb_type[XA];

    for (int si = 0; si < 2; ++si) {
        const Side s = static_cast<Side>(si);
        const Side enemy = other_side(s);
        const int sign = (s == stm) ? 1 : -1;

        // 3a. Non-pawn exposure penalty
        Bitboard exp_bb = (pos.bb_side[s] & non_bv) & bb_att[enemy];
        while (exp_bb.any()) {
            int sq = exp_bb.pop_lsb();
            score -= sign * (piece_material_value(pos.top_piece[sq].type) / 14);
        }

        // 3b. Safe TUONG/SI proximity to own king
        const int okr = kr[si];
        const int okc = kc[si];
        if (okr >= 0) {
            Bitboard safe_ts = (pos.bb_side[s] & ts_mask) & ~bb_att[enemy];
            while (safe_ts.any()) {
                int sq = safe_ts.pop_lsb();
                int dist_ok = std::abs(sq_row(sq) - okr) + std::abs(sq_col(sq) - okc);
                if (dist_ok == 1) score += sign * 32;
                else if (dist_ok == 2) score += sign * 12;
                else if (dist_ok == 3) score += sign * 4;
            }
        }

        // 3c. Vanguard support for SI/KI/XA at rel_row >= 3
        Bitboard vg_bb = pos.bb_side[s] & skx_mask & BB_REL_ROW_GE3[si];
        while (vg_bb.any()) {
            int sq = vg_bb.pop_lsb();
            bool supported = bb_att[s].test_bit(sq);
            if (!supported) {
                int r_front = sq_row(sq) + (s == CAM ? 1 : -1);
                if (in_bounds(r_front, sq_col(sq))) {
                    int fsq = make_sq(r_front, sq_col(sq));
                    if (pos.height[fsq] > 0 && pos.cell_owner(fsq) == s && pos.top_piece[fsq].type == BINH) {
                        supported = true;
                    }
                }
            }
            if (supported) score += sign * w[TERM_SIEGE_VANGUARD_SUPPORT];
        }

        // 3d. Undeveloped KI/SI penalty
        if (developed_units[si] >= 4 && backrank_ki_si[si] > 0) {
            score -= sign * backrank_ki_si[si] * w[TERM_KOMA_UNDEVELOPED_KI_SI];
        }

        // 3e. Siege prisoner pressure & quad units
        if (ksq[enemy] < NUM_SQUARES) {
            int unctrl_empty_ek = (k_zones[enemy] & ~(pos.bb_occ | bb_att[enemy])).popcount();
            if (unctrl_empty_ek >= 2) {
                int att_pris = total_prisoners[si] - pos.prisoners[s][BINH];
                attack_units[si] += std::min(4, att_pris);
            }
        }
        int au = attack_units[si];
        score += sign * (au * w[TERM_SIEGE_UNITS_LINEAR] + (std::min(64, au * au) / 4) * w[TERM_SIEGE_UNITS_QUAD]);

        // 3f. v35 per-side structural bonuses
        if (!pos.has_reserve(s) && lp_on_board[si] >= 2) {
            mg_sum += sign * v35::V35_STRUCT_MG[v35::S_LP_DEPLOYED_SEP];
            eg_sum += sign * v35::V35_STRUCT_EG[v35::S_LP_DEPLOYED_SEP];
        }
        if (phuong_total[si] >= 2) {
            mg_sum += sign * v35::V35_STRUCT_MG[v35::S_DOUBLE_PHUONG];
            eg_sum += sign * v35::V35_STRUCT_EG[v35::S_DOUBLE_PHUONG];
        }
        int lmob = std::min(12, long_mob[si]);
        int pmob = std::min(10, phuong_mob[si]);
        mg_sum += sign * (lmob * v35::V35_STRUCT_MG[v35::S_LONG_OPEN_MOB] + pmob * v35::V35_STRUCT_MG[v35::S_PHUONG_OPEN_MOB]);
        eg_sum += sign * (lmob * v35::V35_STRUCT_EG[v35::S_LONG_OPEN_MOB] + pmob * v35::V35_STRUCT_EG[v35::S_PHUONG_OPEN_MOB]);

        // 3g. King Safety + Siege Drop Hole + v35 King Shelter/Danger (100% branchless bitboard popcounts!)
        if (okr >= 0) {
            const int enemy_prisoners = total_prisoners[enemy];
            const int eff_prisoners = std::min(enemy_prisoners, 4);
            if (okr == (s == CAM ? 0 : 8)) {
                score += sign * 20;
            }
            const Bitboard ring = k_zones[si] ^ k_bb[si];
            const Bitboard own_r = ring & pos.bb_side[s];
            const Bitboard non_own_r = ring & ~pos.bb_side[s];
            const Bitboard empty_r = non_own_r & empty_bb;

            if (k_sheltered[si]) {
                int shield_count = std::min(3, (own_r & BB_REL_ROW_LE1[si]).popcount());
                score += sign * 16 * shield_count;
            }
            score -= sign * 10 * (own_r & bb_att[enemy] & ~bb_att[s]).popcount();

            const int undefended_empty_king_adj = (empty_r & ~bb_att[s]).popcount();
            const int enemy_att_non_own = (non_own_r & bb_att[enemy]).popcount();
            score -= sign * 20 * enemy_att_non_own;
            if (enemy_prisoners >= 2) {
                score -= sign * (25 * enemy_att_non_own + 5 * (empty_r & ~bb_att[enemy]).popcount()) * eff_prisoners;
            } else if (enemy_prisoners == 1) {
                score -= sign * 18 * (empty_r & bb_att[enemy]).popcount();
            }
            if (eff_prisoners > 0 && undefended_empty_king_adj > 0) {
                score -= sign * undefended_empty_king_adj * eff_prisoners * w[TERM_SIEGE_DROP_HOLE];
            }

            int ok_rel = (s == CAM) ? okr : (8 - okr);
            if (ok_rel <= 1 && (okc <= 2 || okc >= 6)) {
                mg_sum += sign * v35::V35_STRUCT_MG[v35::S_CASTLE_WING_KING];
                eg_sum += sign * v35::V35_STRUCT_EG[v35::S_CASTLE_WING_KING];
            } else if (ok_rel >= 1 && okc >= 2 && okc <= 6) {
                if (threat_lp[enemy] > 0 || minors[enemy] >= 2) {
                    mg_sum += sign * v35::V35_STRUCT_MG[v35::S_EXPOSED_CENTER_KING];
                    eg_sum += sign * v35::V35_STRUCT_EG[v35::S_EXPOSED_CENTER_KING];
                }
            }

            // Note: ksq[si] is occupied, so k_zones[si] & ~(bb_occ | bb_att[s]) == empty_r & ~bb_att[s]!
            const int kz_empty_undef = undefended_empty_king_adj;
            int e_pris_majmin = total_prisoners[enemy] - pos.prisoners[enemy][BINH];
            int e_pris_ki = pos.prisoners[enemy][KI];
            int u = std::min(kd_units[si], 28);
            int uq = (u * u) / 16;
            int h_pris = kz_empty_undef * std::min(e_pris_majmin, 4);
            int h_ki = kz_empty_undef * std::min(e_pris_ki, 2);
            mg_sum += sign * (u * v35::V35_STRUCT_MG[v35::S_KD_ATK_LINEAR] +
                              uq * v35::V35_STRUCT_MG[v35::S_KD_ATK_QUAD] +
                              h_pris * v35::V35_STRUCT_MG[v35::S_KD_DROP_HOLES_PRIS] +
                              h_ki * v35::V35_STRUCT_MG[v35::S_KD_KI_DROP_THREAT]);
            eg_sum += sign * (u * v35::V35_STRUCT_EG[v35::S_KD_ATK_LINEAR] +
                              uq * v35::V35_STRUCT_EG[v35::S_KD_ATK_QUAD] +
                              h_pris * v35::V35_STRUCT_EG[v35::S_KD_DROP_HOLES_PRIS] +
                              h_ki * v35::V35_STRUCT_EG[v35::S_KD_KI_DROP_THREAT]);
        }
    }

    // 4. Bitwise Binh connected & passed_adv
    {
        uint16_t ahead_cam = 0;
        int conn_diff = 0;
        int pass_diff = 0;
        const int cam_sign = (stm == CAM) ? 1 : -1;
        for (int r = 8; r >= 0; --r) {
            uint16_t m = binh_row_mask[CAM][r];
            if (m != 0) {
                uint16_t nbr = static_cast<uint16_t>((m << 1) | (m >> 1));
                if (r > 0) {
                    uint16_t p = binh_row_mask[CAM][r - 1];
                    nbr |= static_cast<uint16_t>(p | (p << 1) | (p >> 1));
                }
                if (r < 8) {
                    uint16_t n = binh_row_mask[CAM][r + 1];
                    nbr |= static_cast<uint16_t>(n | (n << 1) | (n >> 1));
                }
                conn_diff += cam_sign * static_cast<int>(std::popcount(static_cast<unsigned>(m & nbr)));
                if (r >= 5) {
                    pass_diff += cam_sign * static_cast<int>(std::popcount(static_cast<unsigned>(m & ~ahead_cam)));
                }
            }
            ahead_cam |= binh_row_mask[XANH][r];
        }
        uint16_t ahead_xanh = 0;
        const int xanh_sign = -cam_sign;
        for (int r = 0; r < 9; ++r) {
            uint16_t m = binh_row_mask[XANH][r];
            if (m != 0) {
                uint16_t nbr = static_cast<uint16_t>((m << 1) | (m >> 1));
                if (r > 0) {
                    uint16_t p = binh_row_mask[XANH][r - 1];
                    nbr |= static_cast<uint16_t>(p | (p << 1) | (p >> 1));
                }
                if (r < 8) {
                    uint16_t n = binh_row_mask[XANH][r + 1];
                    nbr |= static_cast<uint16_t>(n | (n << 1) | (n >> 1));
                }
                conn_diff += xanh_sign * static_cast<int>(std::popcount(static_cast<unsigned>(m & nbr)));
                if ((8 - r) >= 5) {
                    pass_diff += xanh_sign * static_cast<int>(std::popcount(static_cast<unsigned>(m & ~ahead_xanh)));
                }
            }
            ahead_xanh |= binh_row_mask[CAM][r];
        }
        if (conn_diff != 0) {
            mg_sum += conn_diff * v35::V35_STRUCT_MG[v35::S_BINH_CONNECTED];
            eg_sum += conn_diff * v35::V35_STRUCT_EG[v35::S_BINH_CONNECTED];
        }
        if (pass_diff != 0) {
            mg_sum += pass_diff * v35::V35_STRUCT_MG[v35::S_BINH_PASSED_ADV];
            eg_sum += pass_diff * v35::V35_STRUCT_EG[v35::S_BINH_PASSED_ADV];
        }
    }

    const int phase = std::clamp(((phase_raw - 24) * 256) / 50, 0, 256);
    score += (mg_sum * phase + eg_sum * (256 - phase)) / 256;

    // 5. Lazy eval skip + exact O(1)-filtered hanging evaluation
    Bitboard hanging_cands = (pos.bb_side[CAM] & bb_att[XANH]) | (pos.bb_side[XANH] & bb_att[CAM]);
    if (ksq[0] < NUM_SQUARES) hanging_cands.clear_bit(ksq[0]);
    if (ksq[1] < NUM_SQUARES) hanging_cands.clear_bit(ksq[1]);

    out_is_lazy = false;
    if (allow_lazy && g_eval_feats.lazy_eval && (score < alpha - 300 || score > beta + 300)) {
        int ub_ntm1 = 0, ub_ntm2 = 0;
        int ub_stm1 = 0, ub_stm2 = 0;
        Bitboard hb = hanging_cands;
        while (hb.any()) {
            int sq = hb.pop_lsb();
            Side owner = pos.cell_owner(sq);
            int mv = cell_material_value(pos, sq);
            if (owner == ntm) {
                if (mv >= ub_ntm1) { ub_ntm2 = ub_ntm1; ub_ntm1 = mv; }
                else if (mv > ub_ntm2) { ub_ntm2 = mv; }
            } else {
                if (mv >= ub_stm1) { ub_stm2 = ub_stm1; ub_stm1 = mv; }
                else if (mv > ub_stm2) { ub_stm2 = mv; }
            }
        }
        int max_pos_hang = (ub_ntm1 * 3) / 4 + (ub_ntm2 / 4);
        int max_neg_hang = -((ub_stm1 / 4) + (ub_stm2 * 3) / 4);
        if ((score < alpha - 300 && score + max_pos_hang < alpha - 300) ||
            (score > beta + 300 && score + max_neg_hang > beta + 300)) {
            out_is_lazy = true;
#ifdef HOMEI_PROFILE
            if (prof_on) {
                ++prof.eval_lazy_skips;
                auto t_ev1 = std::chrono::steady_clock::now();
                prof.eval_total_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t_ev1 - t_ev0).count()
                );
            }
#endif
            return score;
        }
    }

    int ntm_max1 = 0, ntm_max2 = 0;
    int stm_max1 = 0, stm_max2 = 0;
    Bitboard hb = hanging_cands;
    while (hb.any()) {
        int sq = hb.pop_lsb();
        Side owner = pos.cell_owner(sq);
        Side att_side = other_side(owner);

        int loss = 0;
        if (!bb_att[owner].test_bit(sq)) {
            loss = cell_material_value(pos, sq);
        } else if (pos.height[sq] == 1 && pos.top_piece[sq].type == BINH &&
                   !bb_att2[att_side].test_bit(sq) && !bb_att_stack[att_side].test_bit(sq)) {
            continue;
        } else {
#ifdef HOMEI_PROFILE
            if (prof_on) {
                ++prof.hanging_calls;
                auto th0 = std::chrono::steady_clock::now();
                loss = square_hanging_loss_fast(pos, sq, bb_att);
                auto th1 = std::chrono::steady_clock::now();
                prof.hanging_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(th1 - th0).count()
                );
            } else {
                loss = square_hanging_loss_fast(pos, sq, bb_att);
            }
#else
            loss = square_hanging_loss_fast(pos, sq, bb_att);
#endif
        }
        if (loss <= 0) continue;

        if (owner == ntm) {
            if (loss >= ntm_max1) { ntm_max2 = ntm_max1; ntm_max1 = loss; }
            else if (loss > ntm_max2) { ntm_max2 = loss; }
        } else {
            if (loss >= stm_max1) { stm_max2 = stm_max1; stm_max1 = loss; }
            else if (loss > stm_max2) { stm_max2 = loss; }
        }
    }

    score += ((ntm_max1 * 3) / 4 + (ntm_max2 / 4)) - ((stm_max1 / 4) + (stm_max2 * 3) / 4);

#ifdef HOMEI_PROFILE
    if (prof_on) {
        auto t_ev1 = std::chrono::steady_clock::now();
        prof.eval_total_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t_ev1 - t_ev0).count()
        );
    }
#endif
    return score;
}

EvalBreakdown evaluate_breakdown(const Position& pos) {
    bool is_lazy = false;
    return evaluate_breakdown_impl(pos, -SCORE_INFINITE, SCORE_INFINITE, false, is_lazy);
}

int evaluate(const Position& pos) {
    bool is_lazy = false;
    if (g_eval_fast_eligible) {
        return evaluate_fast(pos, -SCORE_INFINITE, SCORE_INFINITE, false, is_lazy);
    }
    return evaluate_breakdown_impl(pos, -SCORE_INFINITE, SCORE_INFINITE, false, is_lazy).total;
}

int evaluate_maybe_lazy(const Position& pos, int alpha, int beta, bool allow_lazy, bool& out_is_lazy) {
    if (g_eval_fast_eligible) {
        return evaluate_fast(pos, alpha, beta, allow_lazy, out_is_lazy);
    }
    return evaluate_breakdown_impl(pos, alpha, beta, allow_lazy, out_is_lazy).total;
}

int evaluate_v35_extract(const Position& pos, int& out_non_v35_base, v35::V35Trace& out_trace) {
    bool is_lazy = false;
    int v35_delta = 0;
    EvalBreakdown bd = evaluate_breakdown_impl(pos, -SCORE_INFINITE, SCORE_INFINITE, false, is_lazy, &out_trace, &v35_delta);
    out_non_v35_base = bd.total - v35_delta;
    return bd.total;
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
            uint8_t sz = pos.height[src_sq];
            if (sz == 0) continue;
            Piece stack_buf[MAX_STACK_SIZE];
            pos.cell_copy_stack(src_sq, stack_buf);
            for (uint8_t i = 0; i < sz; ++i) {
                Piece p = stack_buf[i];
                if (can_rotate(p.type)) {
                    if (p.facing == 1) p.facing = 3;
                    else if (p.facing == 3) p.facing = 1;
                }
                out.cell_push(dst_sq, p);
                if (p.type == VUONG) {
                    out.king_sq[p.owner] = static_cast<uint8_t>(dst_sq);
                }
            }
        }
    }
    out.sync_bitboards();
    refresh_zobrist(out);
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
            uint8_t sz = pos.height[src_sq];
            if (sz == 0) continue;
            Piece stack_buf[MAX_STACK_SIZE];
            pos.cell_copy_stack(src_sq, stack_buf);
            for (uint8_t i = 0; i < sz; ++i) {
                Piece p = stack_buf[i];
                p.owner = other_side(static_cast<Side>(p.owner));
                if (can_rotate(p.type)) {
                    p.facing = static_cast<uint8_t>((p.facing + 2) & 3);
                } else {
                    p.facing = initial_facing(static_cast<Side>(p.owner));
                }
                out.cell_push(dst_sq, p);
                if (p.type == VUONG) {
                    out.king_sq[p.owner] = static_cast<uint8_t>(dst_sq);
                }
            }
        }
    }
    out.sync_bitboards();
    refresh_zobrist(out);
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

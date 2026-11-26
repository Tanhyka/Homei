#pragma once

#include "attack.h"
#include "bitboard.h"
#include "eval_weights_v35.h"
#include "geometry.h"
#include "movegen.h"
#include "types.h"
#include <algorithm>
#include <cstdint>

namespace homei::v35 {

inline int sym_pst_col(int col) {
    return (col <= 4) ? col : (8 - col);
}

// Mirror & 180-flip invariant facing index (0..3):
// 0=forward, 1=backward, 2=toward_center (or col 4 sideways), 3=toward_edge.
inline int sym_pst_facing(Side s, int col, uint8_t pt, uint8_t f) {
    if (!can_rotate(pt)) return 0;
    uint8_t fwd = (s == CAM) ? 2 : 0;
    uint8_t bwd = (s == CAM) ? 0 : 2;
    if (f == fwd) return 0;
    if (f == bwd) return 1;
    if (col < 4) return (f == 1) ? 2 : 3;
    if (col > 4) return (f == 3) ? 2 : 3;
    return 2;
}

inline bool is_edge_dead_facing(int r, int col, uint8_t f) {
    return (f == 0 && r == 0) || (f == 2 && r == 8) || (f == 1 && col == 8) || (f == 3 && col == 0);
}

inline bool is_king_sheltered(Side s, int okr, int okc) {
    if (okr < 0) return false;
    int rel_r = (s == CAM) ? okr : (8 - okr);
    if (rel_r == 0) return true;
    return (rel_r == 1 && (okc <= 2 || okc >= 6));
}

struct V35TraceItem {
    uint16_t idx;
    int16_t val;
};

struct V35Trace {
    int phase = 0;
    V35TraceItem items[256]{};
    int count = 0;

    void clear() { count = 0; }
};

template <bool RecTrace>
inline int evaluate_v35_tapered_impl(
    const Position& pos,
    const Bitboard bb_att[2],
    const int long_mob[2],
    const int phuong_mob[2],
    const int kd_units_pre[2],
    const int* occ_sqs,
    int occ_count,
    V35Trace* out_trace
) {
    Side stm = static_cast<Side>(pos.side_to_move);
    Side ntm = other_side(stm);

    int mg_sum = 0;
    int eg_sum = 0;

    // Only allocate & zero the 2,950-byte trace buffer when actually extracting Texel traces!
    int16_t trace_buf_storage[RecTrace ? V35_TOTAL_FEATS : 1]{};

    constexpr int PHASE_W[8] = {0, 3, 2, 3, 2, 4, 4, 1};
    int phase_raw = 0;
    int mat_cnt[2][8]{};
    int phuong_total[2]{};
    if (pos.reserve[CAM] & RESERVE_LONG) { phase_raw += 4; mat_cnt[CAM][LONG]++; }
    if (pos.reserve[CAM] & RESERVE_PHUONG) { phase_raw += 4; mat_cnt[CAM][PHUONG]++; phuong_total[CAM]++; }
    if (pos.reserve[XANH] & RESERVE_LONG) { phase_raw += 4; mat_cnt[XANH][LONG]++; }
    if (pos.reserve[XANH] & RESERVE_PHUONG) { phase_raw += 4; mat_cnt[XANH][PHUONG]++; phuong_total[XANH]++; }

    int king_sq_arr[2] = {pos.king_sq[0], pos.king_sq[1]};
    int king_r[2] = {
        (king_sq_arr[0] < NUM_SQUARES) ? sq_row(king_sq_arr[0]) : -1,
        (king_sq_arr[1] < NUM_SQUARES) ? sq_row(king_sq_arr[1]) : -1
    };
    int king_c[2] = {
        (king_sq_arr[0] < NUM_SQUARES) ? sq_col(king_sq_arr[0]) : -1,
        (king_sq_arr[1] < NUM_SQUARES) ? sq_col(king_sq_arr[1]) : -1
    };

    int struct_feats[2][V35_STRUCT_COUNT]{};
    int kd_units[2] = {kd_units_pre[0], kd_units_pre[1]};
    int lp_on_board[2]{};

    uint16_t binh_row_mask[2][9]{};
    uint8_t binh_r_list[2][20]{};
    uint8_t binh_c_list[2][20]{};
    int binh_cnt[2]{};

    for (int oi = 0; oi < occ_count; ++oi) {
        int sq = occ_sqs[oi];
        uint8_t csize = pos.height[sq];
        Side s = pos.cell_owner(sq);
        int sign = (s == stm) ? 1 : -1;
        int r = sq_row(sq);
        int col = sq_col(sq);
        int rel_r = (s == CAM) ? r : (8 - r);
        const Piece top = pos.top_piece[sq];

        bool has_long = false;
        bool has_phuong = false;
        {
            uint8_t pt = top.type;
            phase_raw += PHASE_W[pt];
            mat_cnt[s][pt]++;
            if (pt == LONG) has_long = true;
            else if (pt == PHUONG) { has_phuong = true; phuong_total[s]++; }
        }
        if (csize >= 2) {
            for (uint8_t idx = pos.buried_head[sq]; idx != BURIED_NONE; idx = pos.buried_next[idx]) {
                uint8_t pt = pos.buried_piece[idx].type;
                phase_raw += PHASE_W[pt];
                mat_cnt[s][pt]++;
                if (pt == LONG) has_long = true;
                else if (pt == PHUONG) { has_phuong = true; phuong_total[s]++; }
            }
        }

        if (top.type == LONG || (has_long && !has_phuong)) lp_on_board[s]++;
        if (top.type == PHUONG || (has_phuong && !has_long)) lp_on_board[s]++;

        int sc = sym_pst_col(col);
        int sf = sym_pst_facing(s, col, top.type, top.facing);
        int sq45 = rel_r * 5 + sc;
        mg_sum += sign * static_cast<int>(V35_PST_MG[top.type][sf][sq45]);
        eg_sum += sign * static_cast<int>(V35_PST_EG[top.type][sf][sq45]);
        if constexpr (RecTrace) {
            int tidx = static_cast<int>(top.type) * 180 + sf * 45 + sq45;
            trace_buf_storage[tidx] = static_cast<int16_t>(trace_buf_storage[tidx] + sign);
        }

        if (top.type == BINH) {
            binh_row_mask[s][r] |= static_cast<uint16_t>(1u << col);
            if (binh_cnt[s] < 20) {
                binh_r_list[s][binh_cnt[s]] = static_cast<uint8_t>(r);
                binh_c_list[s][binh_cnt[s]] = static_cast<uint8_t>(col);
                ++binh_cnt[s];
            }
        }

        if (csize >= 2) {
            if (has_long && has_phuong && rel_r <= 1) {
                struct_feats[s][S_LP_HOME_STACK] += 1;
            }
            if (top.type == BINH && !is_edge_dead_facing(r, col, top.facing)) {
                bool has_ki = false, has_sl = false;
                for (uint8_t idx = pos.buried_head[sq]; idx != BURIED_NONE; idx = pos.buried_next[idx]) {
                    uint8_t bt = pos.buried_piece[idx].type;
                    if (bt == KI) has_ki = true;
                    else if (bt == LONG || bt == PHUONG || bt == XA) has_sl = true;
                }
                if (has_ki && rel_r <= 6) struct_feats[s][S_CARRIER_BK] += 1;
                if (has_sl && rel_r >= 2 && rel_r <= 7) struct_feats[s][S_CARRIER_SLIDER] += 1;
            }
            if (rel_r <= 2 && (top.type == TUONG || top.type == SI || top.type == BINH)) {
                for (uint8_t idx = pos.buried_head[sq]; idx != BURIED_NONE; idx = pos.buried_next[idx]) {
                    uint8_t bt = pos.buried_piece[idx].type;
                    if (bt == TUONG || bt == SI) {
                        struct_feats[s][S_STACK_DEFENSIVE] += 1;
                        break;
                    }
                }
            }
        }

        int okr = king_r[s];
        int okc = king_c[s];
        if (okr >= 0 && sq != king_sq_arr[s]) {
            int ok_rel = (s == CAM) ? okr : (8 - okr);
            int d_king = std::max(std::abs(r - okr), std::abs(col - okc));
            if (d_king <= 2) {
                if (ok_rel <= 1 && (top.type == TUONG || top.type == SI)) {
                    struct_feats[s][S_CASTLE_WALL_TS] += 1;
                }
                if (d_king <= 1) {
                    if (top.type == BINH && !is_edge_dead_facing(r, col, top.facing) && is_king_sheltered(s, okr, okc)) {
                        struct_feats[s][S_CASTLE_B_SHIELD] += 1;
                    }
                    if (top.type == TUONG || top.type == SI || top.type == KI) {
                        struct_feats[s][S_KING_RING_DEFENDERS] += 1;
                    }
                }
            }
        }

        if (top.type == XA) {
            constexpr int dr4[4] = {-1, 0, 1, 0};
            constexpr int dc4[4] = {0, 1, 0, -1};
            int nr = r + dr4[top.facing];
            int nc = col + dc4[top.facing];
            if (in_bounds(nr, nc)) {
                int asq = make_sq(nr, nc);
                if (pos.height[asq] == 0 || pos.cell_owner(asq) != s) {
                    struct_feats[s][S_XA_OPEN_LANE] += 1;
                }
            }
        }
    }

    int phase = std::clamp(((phase_raw - 24) * 256) / 50, 0, 256);

    uint16_t enemy_binh_ahead_for_cam[9]{};
    {
        uint16_t acc = 0;
        for (int r = 8; r >= 0; --r) {
            enemy_binh_ahead_for_cam[r] = acc;
            acc |= binh_row_mask[XANH][r];
        }
    }
    uint16_t enemy_binh_ahead_for_xanh[9]{};
    {
        uint16_t acc = 0;
        for (int r = 0; r < 9; ++r) {
            enemy_binh_ahead_for_xanh[r] = acc;
            acc |= binh_row_mask[CAM][r];
        }
    }

    for (int si = 0; si < 2; ++si) {
        Side s = static_cast<Side>(si);
        Side enemy = other_side(s);

        struct_feats[si][S_MAT_TUONG] = mat_cnt[si][TUONG];
        struct_feats[si][S_MAT_SI] = mat_cnt[si][SI];
        struct_feats[si][S_MAT_KI] = mat_cnt[si][KI];
        struct_feats[si][S_MAT_XA] = mat_cnt[si][XA];
        struct_feats[si][S_MAT_LONG] = mat_cnt[si][LONG];
        struct_feats[si][S_MAT_PHUONG] = mat_cnt[si][PHUONG];
        struct_feats[si][S_MAT_BINH] = mat_cnt[si][BINH];

        struct_feats[si][S_PRIS_TUONG] = pos.prisoners[s][TUONG];
        struct_feats[si][S_PRIS_SI] = pos.prisoners[s][SI];
        struct_feats[si][S_PRIS_KI] = pos.prisoners[s][KI];
        struct_feats[si][S_PRIS_XA] = pos.prisoners[s][XA];
        struct_feats[si][S_PRIS_LONG] = pos.prisoners[s][LONG];
        struct_feats[si][S_PRIS_PHUONG] = pos.prisoners[s][PHUONG];
        struct_feats[si][S_PRIS_BINH] = pos.prisoners[s][BINH];

        if (!pos.has_reserve(s) && lp_on_board[si] >= 2) {
            struct_feats[si][S_LP_DEPLOYED_SEP] = 1;
        }
        if (phuong_total[si] >= 2) {
            struct_feats[si][S_DOUBLE_PHUONG] = 1;
        }

        struct_feats[si][S_LONG_OPEN_MOB] = std::min(12, long_mob[si]);
        struct_feats[si][S_PHUONG_OPEN_MOB] = std::min(10, phuong_mob[si]);

        const uint16_t* row_masks = binh_row_mask[si];
        const uint16_t* ahead_masks = (s == CAM) ? enemy_binh_ahead_for_cam : enemy_binh_ahead_for_xanh;
        for (int bi = 0; bi < binh_cnt[si]; ++bi) {
            int r = binh_r_list[si][bi];
            int col = binh_c_list[si][bi];
            uint16_t col_bit = static_cast<uint16_t>(1u << col);
            uint16_t adj3 = static_cast<uint16_t>(col_bit | (col_bit << 1) | (col_bit >> 1));
            uint16_t side2 = static_cast<uint16_t>((col_bit << 1) | (col_bit >> 1));
            bool conn = (row_masks[r] & side2) != 0 ||
                        (r > 0 && (row_masks[r - 1] & adj3) != 0) ||
                        (r < 8 && (row_masks[r + 1] & adj3) != 0);
            if (conn) struct_feats[si][S_BINH_CONNECTED] += 1;

            int rel_r = (s == CAM) ? r : (8 - r);
            if (rel_r >= 5 && (ahead_masks[r] & col_bit) == 0) {
                struct_feats[si][S_BINH_PASSED_ADV] += 1;
            }
        }

        int okr = king_r[si];
        int okc = king_c[si];
        if (okr >= 0) {
            int ok_rel = (s == CAM) ? okr : (8 - okr);
            if (ok_rel <= 1 && (okc <= 2 || okc >= 6)) {
                struct_feats[si][S_CASTLE_WING_KING] = 1;
            } else if (ok_rel >= 1 && okc >= 2 && okc <= 6) {
                int enemy_threat_mat = (mat_cnt[1 - si][LONG] + mat_cnt[1 - si][PHUONG] > 0) ? 2 : 0;
                int enemy_minors = mat_cnt[1 - si][TUONG] + mat_cnt[1 - si][SI] + mat_cnt[1 - si][KI] + mat_cnt[1 - si][XA];
                if (enemy_threat_mat >= 1 || enemy_minors >= 2) {
                    struct_feats[si][S_EXPOSED_CENTER_KING] = 1;
                }
            }

            int kz_empty_undefended = (BB_KING_ZONE[king_sq_arr[si]] & ~(pos.bb_occ | bb_att[s])).popcount();

            int e_pris_majmin = pos.prisoners[enemy][TUONG] + pos.prisoners[enemy][SI] +
                                pos.prisoners[enemy][KI] + pos.prisoners[enemy][XA] +
                                pos.prisoners[enemy][LONG] + pos.prisoners[enemy][PHUONG];
            int e_pris_ki = pos.prisoners[enemy][KI];

            int u = std::min(kd_units[si], 28);
            struct_feats[si][S_KD_ATK_LINEAR] = u;
            struct_feats[si][S_KD_ATK_QUAD] = (u * u) / 16;
            struct_feats[si][S_KD_DROP_HOLES_PRIS] = kz_empty_undefended * std::min(e_pris_majmin, 4);
            struct_feats[si][S_KD_KI_DROP_THREAT] = kz_empty_undefended * std::min(e_pris_ki, 2);
        }
    }

    struct_feats[stm][S_TEMPO] = 1;

    for (int k = 0; k < V35_STRUCT_COUNT; ++k) {
        int net = struct_feats[stm][k] - struct_feats[ntm][k];
        if (net != 0) {
            mg_sum += net * static_cast<int>(V35_STRUCT_MG[k]);
            eg_sum += net * static_cast<int>(V35_STRUCT_EG[k]);
            if constexpr (RecTrace) {
                trace_buf_storage[V35_PST_COUNT + k] = static_cast<int16_t>(net);
            }
        }
    }

    if constexpr (RecTrace) {
        out_trace->phase = phase;
        out_trace->count = 0;
        for (int i = 0; i < V35_TOTAL_FEATS; ++i) {
            if (trace_buf_storage[i] != 0 && out_trace->count < 256) {
                out_trace->items[out_trace->count++] = {static_cast<uint16_t>(i), trace_buf_storage[i]};
            }
        }
    }

    int mixed = mg_sum * phase + eg_sum * (256 - phase);
    return mixed / 256;
}

inline int evaluate_v35_tapered(
    const Position& pos,
    const Bitboard bb_att[2],
    const int long_mob[2],
    const int phuong_mob[2],
    const int kd_units_pre[2],
    const int* occ_sqs,
    int occ_count,
    V35Trace* out_trace = nullptr
) {
    if (out_trace != nullptr) {
        return evaluate_v35_tapered_impl<true>(pos, bb_att, long_mob, phuong_mob, kd_units_pre, occ_sqs, occ_count, out_trace);
    }
    return evaluate_v35_tapered_impl<false>(pos, bb_att, long_mob, phuong_mob, kd_units_pre, occ_sqs, occ_count, nullptr);
}

} // namespace homei::v35

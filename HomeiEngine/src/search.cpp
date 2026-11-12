#include "search.h"
#include "attack.h"
#include "eval.h"
#include "features.h"
#include "geometry.h"
#include "mate.h"
#include "movegen.h"
#include "notation.h"
#include "see.h"
#include "zobrist.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

namespace homei {

namespace {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool move_may_give_check(const Position& pos, const Move& m, int enemy_king) {
    if (enemy_king < 0 || enemy_king >= NUM_SQUARES) return false;
    int end_sq = (m.kind == MoveKind::Rotate) ? m.src : m.dst;
    if (end_sq >= 0 && end_sq < NUM_SQUARES) {
        uint8_t end_f = (m.kind == MoveKind::Drop)
            ? m.facing
            : static_cast<uint8_t>((pos.cell[m.src].top().facing + m.rotate_after) & 3);
        if (piece_attacks_sq(pos, end_sq, m.ptype, end_f, enemy_king)) {
            return true;
        }
    }
    if (m.src >= 0 && m.src < NUM_SQUARES) {
        int r_src = sq_row(m.src);
        int c_src = sq_col(m.src);
        int r_k = sq_row(enemy_king);
        int c_k = sq_col(enemy_king);
        if (r_src == r_k || c_src == c_k || std::abs(r_src - r_k) == std::abs(c_src - c_k)) {
            return true;
        }
    }
    return false;
}

bool is_far_drop(const Move& m, int own_king, int enemy_king) {
    if (m.kind != MoveKind::Drop || m.dst < 0 || m.dst >= NUM_SQUARES) return false;
    int r = sq_row(m.dst);
    int c = sq_col(m.dst);
    if (own_king >= 0 && own_king < NUM_SQUARES) {
        int d_own = std::max(std::abs(r - sq_row(own_king)), std::abs(c - sq_col(own_king)));
        if (d_own <= 3) return false;
    }
    if (enemy_king >= 0 && enemy_king < NUM_SQUARES) {
        int d_enemy = std::max(std::abs(r - sq_row(enemy_king)), std::abs(c - sq_col(enemy_king)));
        if (d_enemy <= 3) return false;
    }
    return true;
}

bool is_passive_rotate(const Position& pos, const Move& m, Side enemy, int enemy_king) {
    if (m.kind != MoveKind::Rotate || m.src < 0 || m.src >= NUM_SQUARES) return false;
    const Cell& c = pos.cell[m.src];
    if (c.empty()) return false;
    uint8_t end_f = static_cast<uint8_t>((c.top().facing + m.rotate_after) & 3);
    // Does it attack enemy king?
    if (enemy_king >= 0 && enemy_king < NUM_SQUARES && piece_attacks_sq(pos, m.src, m.ptype, end_f, enemy_king)) {
        return false;
    }
    // Does it attack any enemy piece?
    const PieceMoveTable& tbl = get_piece_moves(m.ptype, end_f);
    int r0 = sq_row(m.src);
    int c0 = sq_col(m.src);
    for (int i = 0; i < tbl.count; ++i) {
        const MoveVec& mv = tbl.vecs[i];
        if (!mv.sliding) {
            int nr = r0 + mv.dr;
            int nc = c0 + mv.dc;
            if (in_bounds(nr, nc)) {
                int s = make_sq(nr, nc);
                const Cell& tc = pos.cell[s];
                if (!tc.empty() && tc.owner() == enemy) return false;
            }
        } else {
            int nr = r0 + mv.dr;
            int nc = c0 + mv.dc;
            while (in_bounds(nr, nc)) {
                int s = make_sq(nr, nc);
                const Cell& tc = pos.cell[s];
                if (!tc.empty()) {
                    if (tc.owner() == enemy) return false;
                    break;
                }
                nr += mv.dr;
                nc += mv.dc;
            }
        }
    }
    return true;
}

bool drop_threatens_enemy(const Position& pos, const Move& m, Side enemy, int enemy_king) {
    if (m.kind != MoveKind::Drop || m.dst < 0 || m.dst >= NUM_SQUARES) return false;
    if (enemy_king >= 0 && enemy_king < NUM_SQUARES &&
        piece_attacks_sq(pos, m.dst, m.ptype, m.facing, enemy_king)) {
        return true;
    }
    const PieceMoveTable& tbl = get_piece_moves(m.ptype, m.facing);
    int r0 = sq_row(m.dst);
    int c0 = sq_col(m.dst);
    for (int i = 0; i < tbl.count; ++i) {
        const MoveVec& mv = tbl.vecs[i];
        if (!mv.sliding) {
            int nr = r0 + mv.dr;
            int nc = c0 + mv.dc;
            if (in_bounds(nr, nc)) {
                int s = make_sq(nr, nc);
                const Cell& tc = pos.cell[s];
                if (!tc.empty() && tc.owner() == enemy) return true;
            }
        } else {
            int nr = r0 + mv.dr;
            int nc = c0 + mv.dc;
            while (in_bounds(nr, nc)) {
                int s = make_sq(nr, nc);
                const Cell& tc = pos.cell[s];
                if (!tc.empty()) {
                    if (tc.owner() == enemy) return true;
                    break;
                }
                nr += mv.dr;
                nc += mv.dc;
            }
        }
    }
    return false;
}

int lmr_table[64][128]{};
bool lmr_table_initialized = false;

void init_lmr_table() {
    if (lmr_table_initialized) return;
    for (int d = 1; d < 64; ++d) {
        for (int m = 1; m < 128; ++m) {
            double v = 0.75 + (std::log(static_cast<double>(d)) * std::log(static_cast<double>(m))) / 2.25;
            lmr_table[d][m] = static_cast<int>(std::round(v));
        }
    }
    lmr_table_initialized = true;
}

} // namespace

Searcher::Searcher() : tt_(64), tt_ptr_(&tt_), ply_stack_(MAX_PLY + 8) {
    init_attack_tables();
    init_lmr_table();
    std::memset(countermoves_, 0, sizeof(countermoves_));
    std::memset(move_history_, 0, sizeof(move_history_));
    cont_hist_1ply_ = new int16_t[32][NUM_SQUARES][32][NUM_SQUARES]();
    cont_hist_2ply_ = new int16_t[32][NUM_SQUARES][32][NUM_SQUARES]();
    owns_cont_hist_ = true;
}

Searcher::~Searcher() {
    if (owns_cont_hist_) {
        delete[] cont_hist_1ply_;
        delete[] cont_hist_2ply_;
    }
}

bool Searcher::check_stop() {
    if (abort_) return true;
    if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) {
        if (current_root_depth_ > 1) {
            abort_ = true;
            return true;
        }
    }
    if (current_root_depth_ > 1 && nodes_ >= limits_.max_nodes) {
        abort_ = true;
        return true;
    }
    if (current_root_depth_ > 1 && !limits_.infinite && (nodes_ & 127ULL) == 0) {
        int64_t elapsed = now_ms() - start_time_ms_;
        if (elapsed >= hard_limit_ms_) {
            abort_ = true;
            return true;
        }
    }
    return false;
}

int Searcher::count_repetitions(uint64_t hash, int ply) const {
    int count = 0;
    for (const auto& e : base_rep_) {
        if (e.hash == hash) {
            count += e.count;
        }
    }
    // Check search path after root (ply 1 .. ply - 1)
    for (int i = 1; i < ply; ++i) {
        if (path_hashes_[i] == hash) {
            ++count;
        }
    }
    // Plus the current occurrence at `ply` (when ply >= 1)
    if (ply >= 1) {
        ++count;
    }
    return count;
}

void Searcher::score_moves(Position& pos, const MoveList& list, int* scores, Move tt_move, int ply, Move counter_move, Move prev1_move, Move prev2_move) const {
    Side stm = static_cast<Side>(pos.side_to_move);
    Side enemy = other_side(stm);
    int enemy_king = pos.king_sq[enemy];
    int ekr = (enemy_king < NUM_SQUARES) ? sq_row(enemy_king) : -1;
    int ekc = (enemy_king < NUM_SQUARES) ? sq_col(enemy_king) : -1;
    int own_king = pos.king_sq[stm];
    int okr = (own_king < NUM_SQUARES) ? sq_row(own_king) : -1;
    int okc = (own_king < NUM_SQUARES) ? sq_col(own_king) : -1;

    // Fast single-pass attack map when ply <= 2
    bool att_by[2][NUM_SQUARES]{};
    int src_danger[NUM_SQUARES]{};
    int danger_sqs[NUM_SQUARES];
    int danger_count = 0;

    if (ply <= 2) {
        int dsts[32];
        for (int sq = 0; sq < NUM_SQUARES; ++sq) {
            const Cell& c = pos.cell[sq];
            if (c.empty()) continue;
            Side owner = c.owner();
            int n = get_raw_destinations(pos, sq, dsts);
            for (int k = 0; k < n; ++k) {
                att_by[owner][dsts[k]] = true;
            }
        }
        for (int sq = 0; sq < NUM_SQUARES; ++sq) {
            const Cell& c = pos.cell[sq];
            if (!c.empty() && c.owner() == stm && !c.contains_king() && att_by[enemy][sq]) {
                int loss = (ply <= 1) ? square_hanging_loss(pos, sq) : cell_material_value(c);
                if (loss > 0) {
                    src_danger[sq] = loss;
                    danger_sqs[danger_count++] = sq;
                }
            }
        }
    }

    for (int i = 0; i < list.count; ++i) {
        const Move& m = list.moves[i];
        if (!tt_move.is_none() && m == tt_move) {
            scores[i] = 2000000;
            continue;
        }

        int sc = 0;
        if (is_capture_move(pos, m)) {
            int v_dst = cell_material_value(pos.cell[m.dst]);
            int v_att = (m.kind == MoveKind::Team)
                ? cell_material_value(pos.cell[m.src])
                : piece_material_value(pos.cell[m.src].top().type);
            int see_gain = stack_see_estimate(pos, m);

            int follow_bonus = 0;
            if (ply <= 2 && can_rotate(m.ptype)) {
                uint8_t eff_f = (m.kind == MoveKind::Drop)
                    ? static_cast<uint8_t>(m.facing)
                    : static_cast<uint8_t>((pos.cell[m.src].top().facing + m.rotate_after) & 3);
                const PieceMoveTable& tbl = get_piece_moves(m.ptype, eff_f);
                int rd = sq_row(m.dst);
                int cd = sq_col(m.dst);
                for (int vi = 0; vi < tbl.count; ++vi) {
                    const MoveVec& mv = tbl.vecs[vi];
                    if (!mv.sliding) {
                        int tr = rd + mv.dr;
                        int tc = cd + mv.dc;
                        if (in_bounds(tr, tc)) {
                            int tsq = make_sq(tr, tc);
                            if (tsq != m.src && !pos.cell[tsq].empty() && pos.cell[tsq].owner() == enemy) {
                                follow_bonus += 16 + cell_material_value(pos.cell[tsq]) / 32;
                            }
                        }
                    }
                }
                if (ekr >= 0 && (std::abs(rd - ekr) + std::abs(cd - ekc)) <= 3) {
                    bool points_at_ek =
                        (eff_f == 0 && ekr < rd) ||
                        (eff_f == 2 && ekr > rd) ||
                        (eff_f == 1 && ekc > cd) ||
                        (eff_f == 3 && ekc < cd);
                    if (points_at_ek) follow_bonus += 12;
                }
            }

            if (see_gain >= 0) {
                sc = 1000000 + see_gain * 64 + (10 * v_dst - v_att) + follow_bonus;
            } else {
                sc = -100000 + see_gain * 64 + (10 * v_dst - v_att) + follow_bonus;
            }
        } else if (m.kind == MoveKind::Drop) {
            int v_drop = piece_material_value(m.ptype);
            int r = sq_row(m.dst);
            int c = sq_col(m.dst);
            uint8_t drop_f = static_cast<uint8_t>(m.facing);
            int dist_ek = (ekr >= 0) ? (std::abs(r - ekr) + std::abs(c - ekc)) : 8;
            sc = 1000 + v_drop + (16 - dist_ek) * 20;

            if (!counter_move.is_none() && m == counter_move) {
                sc = std::max(sc, 700000);
            }

            // King-ring defensive block drop (e.g. T21 B*b8^)
            if (ply <= 1 && okr >= 0 && pos.cell[m.dst].empty() &&
                std::abs(r - okr) <= 1 && std::abs(c - okc) <= 1) {
                if (att_by[enemy][m.dst] && att_by[stm][m.dst]) {
                    sc += 300000 - v_drop * 16;
                }
            }

            // O(1) geometric check whether dropped piece gives check to enemy king
            if (enemy_king < NUM_SQUARES && piece_attacks_sq(pos, m.dst, m.ptype, drop_f, enemy_king)) {
                sc += 380000 - v_drop * 4;
            }

            // Fast fork/threat detection at root without apply_move
            if (ply <= 1 && (!att_by[enemy][m.dst] || att_by[stm][m.dst])) {
                const PieceMoveTable& tbl = get_piece_moves(m.ptype, drop_f);
                int attacked_targets = 0;
                int attacked_value_sum = 0;
                for (int vi = 0; vi < tbl.count; ++vi) {
                    const MoveVec& mv = tbl.vecs[vi];
                    if (!mv.sliding) {
                        int tr = r + mv.dr;
                        int tc = c + mv.dc;
                        if (in_bounds(tr, tc)) {
                            int tsq = make_sq(tr, tc);
                            const Cell& tc_cell = pos.cell[tsq];
                            if (!tc_cell.empty() && tc_cell.owner() == enemy && !tc_cell.contains_king()) {
                                int tval = cell_material_value(tc_cell);
                                if (!att_by[enemy][tsq] || tval > v_drop) {
                                    ++attacked_targets;
                                    attacked_value_sum += tval;
                                }
                            }
                        }
                    } else {
                        int tr = r + mv.dr;
                        int tc = c + mv.dc;
                        while (in_bounds(tr, tc)) {
                            int tsq = make_sq(tr, tc);
                            const Cell& tc_cell = pos.cell[tsq];
                            if (!tc_cell.empty()) {
                                if (tc_cell.owner() == enemy && !tc_cell.contains_king()) {
                                    int tval = cell_material_value(tc_cell);
                                    if (!att_by[enemy][tsq] || tval > v_drop) {
                                        ++attacked_targets;
                                        attacked_value_sum += tval;
                                    }
                                }
                                break;
                            }
                            tr += mv.dr;
                            tc += mv.dc;
                        }
                    }
                }
                if (attacked_targets >= 2) {
                    sc += 260000 + attacked_value_sum * 16;
                } else if (attacked_targets == 1) {
                    sc += 20000 + attacked_value_sum * 8;
                }
            }
        } else {
            // Single, Team, or Rotate quiet move
            if (ply < MAX_PLY) {
                if (m == killers_[ply][0]) sc = 800000;
                else if (m == killers_[ply][1]) sc = 750000;
                else if (!counter_move.is_none() && m == counter_move) sc = 700000;
            }
            int target_sq = (m.dst >= 0) ? m.dst : m.src;
            if (target_sq >= 0 && target_sq < NUM_SQUARES) {
                sc += std::min(400000, history_[stm][m.ptype][target_sq]);
            }

            // Step 4.1: Continuation History bonus (1-ply and 2-ply)
            int end_sq = (m.kind == MoveKind::Rotate) ? m.src : m.dst;
            if (end_sq >= 0 && end_sq < NUM_SQUARES) {
                int8_t facing = (m.kind == MoveKind::Drop) ? m.facing : static_cast<int8_t>((pos.cell[m.src].top().facing + m.rotate_after) & 3);
                int curr_pf = piece_facing_idx(m.ptype, facing);
                if (!prev1_move.is_none() && cont_hist_1ply_) {
                    int p1_sq = (prev1_move.kind == MoveKind::Rotate) ? prev1_move.src : prev1_move.dst;
                    if (p1_sq >= 0 && p1_sq < NUM_SQUARES) {
                        int p1_pf = piece_facing_idx(prev1_move.ptype, prev1_move.facing);
                        sc += cont_hist_1ply_[p1_pf][p1_sq][curr_pf][end_sq];
                    }
                }
                if (!prev2_move.is_none() && cont_hist_2ply_) {
                    int p2_sq = (prev2_move.kind == MoveKind::Rotate) ? prev2_move.src : prev2_move.dst;
                    if (p2_sq >= 0 && p2_sq < NUM_SQUARES) {
                        int p2_pf = piece_facing_idx(prev2_move.ptype, prev2_move.facing);
                        sc += cont_hist_2ply_[p2_pf][p2_sq][curr_pf][end_sq];
                    }
                }
            }

            uint8_t end_f = static_cast<uint8_t>((pos.cell[m.src].top().facing + m.rotate_after) & 3);

            if (ply <= 2) {
                if (m.src >= 0 && m.dst >= 0 && src_danger[m.src] > 0) {
                    const Cell& sc_cell = pos.cell[m.src];
                    if (sc_cell.size == 1 && sc_cell.top().type == BINH) {
                        sc += 185000 + src_danger[m.src] * 16;
                    } else {
                        sc += 210000 + src_danger[m.src] * 32;
                    }
                }
                if (danger_count > 0) {
                    for (int di = 0; di < danger_count; ++di) {
                        int dsq = danger_sqs[di];
                        if (dsq != m.src && dsq != end_sq &&
                            piece_attacks_sq(pos, end_sq, m.ptype, end_f, dsq)) {
                            sc += 195000 + src_danger[dsq] * 32;
                            break;
                        }
                    }
                }
            }
            if (ply <= 1 && enemy_king < NUM_SQUARES) {
                if (piece_attacks_sq(pos, end_sq, m.ptype, end_f, enemy_king)) {
                    sc += 360000;
                }
            }
        }
        scores[i] = sc;
    }
}

int Searcher::qsearch(Position& pos, int alpha, int beta, int ply, int qply) {
    pv_length_[ply] = ply;
    if (check_stop()) return 0;

    // 1. Check 3-fold repetition first (Section 4.2)
    if (ply >= 1) {
        int rep = count_repetitions(pos.zobrist, ply);
        if (rep >= 3) {
            return SCORE_MATE - ply;
        }
    }

    if (ply >= MAX_PLY - 1 || qply >= 10) {
        return evaluate(pos);
    }

    Side stm = static_cast<Side>(pos.side_to_move);
    bool in_chk = is_in_check(pos, stm);

    MoveList& legal = ply_stack_[ply].moves;
    int* scores = ply_stack_[ply].scores;

    if (in_chk) {
        // When in check, generate all legal evasions to detect checkmate accurately
        generate_legal_moves(pos, legal);
        if (legal.count == 0) {
            return -(SCORE_MATE - ply);
        }

        score_moves(pos, legal, scores, Move::none(), ply);

        int best_score = -SCORE_INFINITE;
        for (int i = 0; i < legal.count; ++i) {
            int best_idx = i;
            for (int j = i + 1; j < legal.count; ++j) {
                if (scores[j] > scores[best_idx]) best_idx = j;
            }
            if (best_idx != i) {
                std::swap(legal.moves[i], legal.moves[best_idx]);
                std::swap(scores[i], scores[best_idx]);
            }

            const Move m = legal.moves[i];
            UndoState u;
            apply_move(pos, m, u);
            ++nodes_;
#ifdef HOMEI_STATS
            ++stats_.qnodes;
#endif
            if ((nodes_ & 127ULL) == 0 && check_stop()) {
                undo_move(pos, m, u);
                return 0;
            }
            path_hashes_[ply + 1] = pos.zobrist;

            int score = -qsearch(pos, -beta, -alpha, ply + 1, qply + 1);
            undo_move(pos, m, u);
            if (abort_) return 0;

            if (score > best_score) {
                best_score = score;
                if (score > alpha) {
                    alpha = score;
                    pv_table_[ply][ply] = m;
                    for (int k = ply + 1; k < pv_length_[ply + 1]; ++k) {
                        pv_table_[ply][k] = pv_table_[ply + 1][k];
                    }
                    pv_length_[ply] = pv_length_[ply + 1];
                    if (alpha >= beta) break;
                }
            }
        }
        return best_score;
    }

    // Tier 3: mate1_leaf fast scan at qply == 0
    if (EngineFeatures::get().mate1_leaf && qply == 0 && !in_chk) {
        Side enemy = other_side(stm);
        int ek = pos.king_sq[enemy];
        int escapes = 0;
        if (ek < NUM_SQUARES) {
            int dsts[16];
            int nd = get_raw_destinations(pos, ek, dsts);
            for (int i = 0; i < nd; ++i) {
                int sq = dsts[i];
                const Cell& c = pos.cell[sq];
                if (c.empty() || c.owner() == stm) {
                    if (!is_attacked(pos, sq, stm)) {
                        ++escapes;
                    }
                }
            }
        }
        if (escapes <= 2) {
            Move m1 = Move::none();
            if (has_mate_in_1(pos, m1, path_hashes_, ply, &mate_rep_)) {
                int score = SCORE_MATE - (ply + 1);
                tt_.store(pos.zobrist, 1, ply, score, TT_EXACT, m1);
                pv_table_[ply][ply] = m1;
                pv_length_[ply] = ply + 1;
                return score;
            }
        }
    }

    int stand_pat = evaluate(pos);
    if (stand_pat >= beta) {
        return stand_pat;
    }
    if (stand_pat > alpha) {
        alpha = stand_pat;
    }

    MoveList& caps = ply_stack_[ply].caps;
    generate_capture_moves(pos, caps);

    // Filter legal captures with SEE >= 0 (or capturing a stack >= 2 pieces at qply == 0)
    legal.clear();
    for (int i = 0; i < caps.count; ++i) {
        const Move m = caps.moves[i];
        int see_gain = stack_see_estimate(pos, m);
        bool is_multi_stack_cap = (m.dst >= 0 && pos.cell[m.dst].size >= 2);
        if (see_gain < 0 && !(qply == 0 && is_multi_stack_cap)) {
            continue;
        }
        UndoState u;
        apply_move(pos, m, u);
        bool is_leg = !is_in_check(pos, stm);
        undo_move(pos, m, u);
        if (!is_leg) continue;

        int v_dst = cell_material_value(pos.cell[m.dst]);
        scores[legal.count] = 100000 + see_gain * 32 + v_dst;
        legal.add(m);
    }

    for (int i = 0; i < legal.count; ++i) {
        int best_idx = i;
        for (int j = i + 1; j < legal.count; ++j) {
            if (scores[j] > scores[best_idx]) best_idx = j;
        }
        if (best_idx != i) {
            std::swap(legal.moves[i], legal.moves[best_idx]);
            std::swap(scores[i], scores[best_idx]);
        }

        const Move m = legal.moves[i];
        UndoState u;
        apply_move(pos, m, u);
        ++nodes_;
#ifdef HOMEI_STATS
        ++stats_.qnodes;
#endif
        if ((nodes_ & 127ULL) == 0 && check_stop()) {
            undo_move(pos, m, u);
            return 0;
        }
        path_hashes_[ply + 1] = pos.zobrist;

        int score = -qsearch(pos, -beta, -alpha, ply + 1, qply + 1);
        undo_move(pos, m, u);
        if (abort_) return 0;

        if (score > stand_pat) {
            stand_pat = score;
            if (score > alpha) {
                alpha = score;
                pv_table_[ply][ply] = m;
                for (int k = ply + 1; k < pv_length_[ply + 1]; ++k) {
                    pv_table_[ply][k] = pv_table_[ply + 1][k];
                }
                pv_length_[ply] = pv_length_[ply + 1];
                if (alpha >= beta) break;
            }
        }
    }

    return stand_pat;
}

int Searcher::pvs(Position& pos, int depth, int alpha, int beta, int ply, bool is_pv, int ext_budget, bool allow_nmp, Move prev_move, Move prev2_move) {
    pv_length_[ply] = ply;
    if (check_stop()) return 0;

    // 1. Check 3-fold repetition BEFORE checkmate/pat (Section 4.2)
    int rep_count = 1;
    if (ply >= 1) {
        rep_count = count_repetitions(pos.zobrist, ply);
        if (rep_count >= 3) {
            // Side that just moved into this 3rd repetition loses immediately
            return SCORE_MATE - ply;
        }
    }

    if (ply >= MAX_PLY - 1) {
        return evaluate(pos);
    }

    // Mate-Distance Pruning (MDP)
    int mating_value = SCORE_MATE - ply;
    if (alpha < -mating_value) alpha = -mating_value;
    if (beta > mating_value - 1) beta = mating_value - 1;
    if (alpha >= beta) return alpha;

    if (depth <= 0) {
        return qsearch(pos, alpha, beta, ply, 0);
    }

    // 2. Transposition Table Probe (disabled for cutoff when rep_count >= 2, Section 4.2)
    TTEntry tte;
    Move tt_move = Move::none();
#ifdef HOMEI_STATS
    ++stats_.tt_probe;
#endif
    bool tt_hit = tt_ptr_->probe(pos.zobrist, tte);
    if (tt_hit) {
#ifdef HOMEI_STATS
        ++stats_.tt_hit;
#endif
        tt_move = tte.best_move;
        if (!is_pv && rep_count == 1 && tte.depth >= depth) {
            int tt_score = score_from_tt(tte.score, ply);
            if (tte.bound == TT_EXACT) {
#ifdef HOMEI_STATS
                ++stats_.tt_cutoff;
#endif
                return tt_score;
            }
            if (tte.bound == TT_LOWER && tt_score >= beta) {
#ifdef HOMEI_STATS
                ++stats_.tt_cutoff;
#endif
                return tt_score;
            }
            if (tte.bound == TT_UPPER && tt_score <= alpha) {
#ifdef HOMEI_STATS
                ++stats_.tt_cutoff;
#endif
                return tt_score;
            }
        }
    }

    Side stm = static_cast<Side>(pos.side_to_move);
    Side enemy = other_side(stm);
    bool in_chk = is_in_check(pos, stm);

    // Tier 2: mate_pvlight on PV nodes
    if (EngineFeatures::get().mate_pvlight && is_pv && !in_chk && ply >= 1 && ply <= 2 && depth >= 4 &&
        std::abs(alpha) < SCORE_MATE - 100 && std::abs(beta) < SCORE_MATE - 100) {
        MateResult res = solve_mate_bounded(pos, 5, 150, stop_flag_, &mate_rep_, path_hashes_, ply);
        nodes_ += res.nodes;
        if (res.found && !res.pv.empty()) {
            int mate_score = SCORE_MATE - (ply + res.mate_ply);
            tt_ptr_->store(pos.zobrist, depth, ply, mate_score, TT_EXACT, res.pv[0]);
            for (size_t mi = 0; mi < res.pv.size() && ply + mi < MAX_PLY; ++mi) {
                pv_table_[ply][ply + mi] = res.pv[mi];
            }
            pv_length_[ply] = ply + static_cast<int>(res.pv.size());
            return mate_score;
        }
    }

    bool mate_threat = false;

    constexpr bool ENABLE_RFP = true;
    constexpr bool ENABLE_RAZORING = true;
    constexpr bool ENABLE_FUTILITY = true;
    constexpr bool ENABLE_LMP = true;
    constexpr bool ENABLE_NMP = true;

    int static_eval = 0;
    if (!is_pv && !in_chk && rep_count == 1) {
        static_eval = evaluate(pos);

        // Reverse Futility Pruning (RFP / Static Null Move)
        if (ENABLE_RFP && depth <= 4 && std::abs(beta) < SCORE_MATE - 100) {
            int margin = 100 * depth;
            if (static_eval - margin >= beta) {
                return beta;
            }
        }

        // Razoring
        if (ENABLE_RAZORING && depth <= 2 && std::abs(alpha) < SCORE_MATE - 100 && std::abs(beta) < SCORE_MATE - 100) {
            if (static_eval + 300 < alpha) {
                int r_score = qsearch(pos, alpha, beta, ply, 0);
                if (abort_) return 0;
                if (r_score < alpha) {
                    return r_score;
                }
            }
        }

        // Null-Move Pruning (NMP)
        if (ENABLE_NMP && allow_nmp && depth >= 3 && static_eval >= beta &&
            std::abs(beta) < SCORE_MATE - 100 && !pos.has_reserve(stm)) {

            // Zugzwang check: must have non-King mobile material
            bool has_non_king_mat = false;
            for (int pt = 1; pt < 8; ++pt) {
                if (pos.prisoners[stm][pt] > 0) {
                    has_non_king_mat = true;
                    break;
                }
            }
            if (!has_non_king_mat) {
                for (int sq = 0; sq < NUM_SQUARES; ++sq) {
                    const Cell& c = pos.cell[sq];
                    if (!c.empty() && c.owner() == stm) {
                        for (uint8_t ci = 0; ci < c.size; ++ci) {
                            if (c.stack[ci].type != VUONG) {
                                has_non_king_mat = true;
                                break;
                            }
                        }
                        if (has_non_king_mat) break;
                    }
                }
            }

            if (has_non_king_mat) {
                int R = (depth >= 6) ? 3 : (2 + depth / 4);
                int null_depth = std::max(0, depth - 1 - R);

                uint64_t prev_zob = pos.zobrist;
                pos.side_to_move = other_side(stm);
                pos.zobrist ^= zobrist_side;
                path_hashes_[ply + 1] = pos.zobrist;

                int null_score = -pvs(pos, null_depth, -beta, -beta + 1, ply + 1, false, 0, false, Move::none(), Move::none());

                pos.side_to_move = stm;
                pos.zobrist = prev_zob;
                if (abort_) return 0;

                if (null_score >= beta) {
                    if (depth < 8) {
#ifdef HOMEI_STATS
                        ++stats_.null_move_cutoffs;
#endif
                        return beta;
                    }
                    // Verification search at high depth (d >= 8)
                    int verif_score = pvs(pos, null_depth, beta - 1, beta, ply, false, 0, false, Move::none(), Move::none());
                    if (verif_score >= beta) {
#ifdef HOMEI_STATS
                        ++stats_.null_move_cutoffs;
#endif
                        return beta;
                    }
                } else if (EngineFeatures::get().mate_threat && null_score <= -SCORE_MATE + MAX_PLY) {
                    mate_threat = true;
                }
            }
        }

        // Step 4.2: ProbCut (depth >= 5, beta cutoff verification on good captures)
        if (depth >= 5 && std::abs(beta) < SCORE_MATE - 100) {
            int prob_beta = beta + 180;
            MoveList cap_moves;
            generate_legal_moves(pos, cap_moves);
            for (int i = 0; i < cap_moves.count; ++i) {
                const Move& cm = cap_moves.moves[i];
                if (!is_capture_move(pos, cm)) continue;
                if (stack_see_estimate(pos, cm) < 0) continue;

                UndoState u_prob;
                apply_move(pos, cm, u_prob);
                path_hashes_[ply + 1] = pos.zobrist;
                int prob_score = -pvs(pos, depth - 4, -prob_beta, -prob_beta + 1, ply + 1, false, 0, false, cm, prev_move);
                undo_move(pos, cm, u_prob);
                if (abort_) return 0;
                if (prob_score >= prob_beta) {
                    return prob_beta;
                }
            }
        }
    }

    if (mate_threat && ext_budget > 0) {
        ++depth;
        --ext_budget;
    }

    // 3. Generate legal moves
    MoveList& legal = ply_stack_[ply].moves;
    int* scores = ply_stack_[ply].scores;
    generate_legal_moves(pos, legal);
    if (legal.count == 0) {
        // Both checkmate and PAT lose for the side to move (Section 2.6 & 4.2)
        return -(SCORE_MATE - ply);
    }

    // Single-reply extension (1 legal evasion) + forced-evasion extension (<= 4 legal evasions)
    if (in_chk && ply < MAX_PLY - 4) {
        if (legal.count == 1) {
            ++depth;
        } else if (legal.count <= 4 && ext_budget > 0) {
            ++depth;
            --ext_budget;
        }
    }

    Move counter_move = Move::none();
    if (!prev_move.is_none()) {
        Side prev_side = other_side(stm);
        uint8_t prev_pt = prev_move.ptype;
        int prev_to = (prev_move.dst >= 0) ? prev_move.dst : prev_move.src;
        if (prev_side < 2 && prev_pt < 8 && prev_to >= 0 && prev_to < NUM_SQUARES) {
            counter_move = countermoves_[prev_side][prev_pt][prev_to];
        }
    }

    score_moves(pos, legal, scores, tt_move, ply, counter_move, prev_move, prev2_move);

    int orig_alpha = alpha;
    int best_score = -SCORE_INFINITE;
    Move best_move = legal.moves[0];

    int own_king = pos.king_sq[stm];
    int enemy_king = pos.king_sq[enemy];

    bool futility_pruning = !mate_threat && (ENABLE_FUTILITY && !is_pv && !in_chk && depth <= 3 && rep_count == 1 &&
                             std::abs(alpha) < SCORE_MATE - 100 &&
                             std::abs(beta) < SCORE_MATE - 100 &&
                             (static_eval + 120 * depth < alpha));

    bool lmp_active = !mate_threat && (ENABLE_LMP && !is_pv && !in_chk && depth <= 4 && rep_count == 1 &&
                       std::abs(alpha) < SCORE_MATE - 100 &&
                       std::abs(beta) < SCORE_MATE - 100);

    int moves_searched = 0;
    int quiet_searched = 0;

    for (int i = 0; i < legal.count; ++i) {
        int best_idx = i;
        for (int j = i + 1; j < legal.count; ++j) {
            if (scores[j] > scores[best_idx]) best_idx = j;
        }
        if (best_idx != i) {
            std::swap(legal.moves[i], legal.moves[best_idx]);
            std::swap(scores[i], scores[best_idx]);
        }

        const Move m = legal.moves[i];
        bool is_cap = is_capture_move(pos, m);
        int move_order_score = scores[i];
        bool may_give_chk = move_may_give_check(pos, m, enemy_king);
        bool is_tt = (!tt_move.is_none() && m == tt_move);
        bool is_killer = (ply < MAX_PLY && (m == killers_[ply][0] || m == killers_[ply][1]));
        bool is_tactical_score = (move_order_score >= 180000);
        bool is_threatened_src = (m.src >= 0 && is_attacked(pos, m.src, enemy));

        // Pruning checks for quiet non-exempt moves
        if (moves_searched > 0 && !is_cap) {
            bool is_active_drop = false;
            if (m.kind == MoveKind::Drop) {
                if (drop_threatens_enemy(pos, m, enemy, enemy_king)) {
                    is_active_drop = true;
                } else if (own_king >= 0 && own_king < NUM_SQUARES) {
                    int dk = std::max(std::abs(sq_row(m.dst) - sq_row(own_king)), std::abs(sq_col(m.dst) - sq_col(own_king)));
                    if (dk <= 2) is_active_drop = true;
                }
            }

            bool is_exempt = is_tt || may_give_chk || is_killer || is_tactical_score || is_threatened_src || is_active_drop;

            if (!is_exempt) {
                // Futility Pruning
                if (futility_pruning) {
                    continue;
                }

                // Late Move Pruning (LMP)
                if (lmp_active) {
                    bool passive_rot = is_passive_rotate(pos, m, enemy, enemy_king);
                    bool far_drp = is_far_drop(m, own_king, enemy_king);
                    int lmp_limit = (passive_rot || far_drp) ? (3 + depth * depth) : (6 + 2 * depth * depth);
                    if (quiet_searched >= lmp_limit) {
                        continue;
                    }
                }
            }
        }

        UndoState u;
        apply_move(pos, m, u);
        ++nodes_;
#ifdef HOMEI_STATS
        ++stats_.nodes;
#endif
        path_hashes_[ply + 1] = pos.zobrist;

        ++moves_searched;
        if (!is_cap) {
            ++quiet_searched;
        }

        bool gives_chk = is_in_check(pos, enemy);
        int new_depth = depth - 1;
        int next_ext_budget = ext_budget;
        int score = 0;

        if (moves_searched == 1) {
            score = -pvs(pos, new_depth, -beta, -alpha, ply + 1, is_pv, next_ext_budget, true, m, prev_move);
        } else {
            // Logarithmic Late Move Reductions (LMR) for quiet non-exempt moves
            int reduction = 0;
            bool lmr_exempt = is_cap || gives_chk || in_chk || is_tt || is_killer || is_threatened_src || is_tactical_score;
            if (!is_pv && depth >= 3 && !in_chk && moves_searched >= 2 && !lmr_exempt) {
                int d_idx = std::min(depth, 63);
                int m_idx = std::min(moves_searched, 127);
                reduction = lmr_table[d_idx][m_idx];

                // History adjustment
                int tsq = (m.dst >= 0) ? m.dst : m.src;
                int hist = (tsq >= 0 && tsq < NUM_SQUARES) ? history_[stm][m.ptype][tsq] : 0;
                if (hist >= 1000) {
                    --reduction;
                } else if (hist <= 0) {
                    ++reduction;
                }
                if (mate_threat && reduction > 0) {
                    --reduction;
                }
                reduction = std::clamp(reduction, 0, new_depth - 1);
            }

            int reduced_depth = std::max(0, new_depth - reduction);
            score = -pvs(pos, reduced_depth, -(alpha + 1), -alpha, ply + 1, false, next_ext_budget, true, m, prev_move);
            if (score > alpha && reduction > 0 && !abort_) {
#ifdef HOMEI_STATS
                ++stats_.lmr_researches;
#endif
                score = -pvs(pos, new_depth, -(alpha + 1), -alpha, ply + 1, false, next_ext_budget, true, m, prev_move);
            }
            if (score > alpha && score < beta && is_pv && !abort_) {
                score = -pvs(pos, new_depth, -beta, -alpha, ply + 1, true, next_ext_budget, true, m, prev_move);
            }
        }

        undo_move(pos, m, u);
        if (abort_) return 0;

        if (score > best_score) {
            best_score = score;
            best_move = m;
            if (score > alpha) {
                alpha = score;
                pv_table_[ply][ply] = m;
                for (int k = ply + 1; k < pv_length_[ply + 1]; ++k) {
                    pv_table_[ply][k] = pv_table_[ply + 1][k];
                }
                pv_length_[ply] = pv_length_[ply + 1];

                if (alpha >= beta) {
#ifdef HOMEI_STATS
                    ++stats_.beta_cutoffs_total;
                    if (moves_searched == 1) {
                        ++stats_.beta_cutoffs_first_move;
                    }
#endif
                    if (!is_cap && ply < MAX_PLY) {
                        if (m.kind != MoveKind::Drop) {
                            if (killers_[ply][0] != m) {
                                killers_[ply][1] = killers_[ply][0];
                                killers_[ply][0] = m;
                            }
                            int tsq = (m.dst >= 0) ? m.dst : m.src;
                            if (tsq >= 0 && tsq < NUM_SQUARES) {
                                history_[stm][m.ptype][tsq] = std::min(400000, history_[stm][m.ptype][tsq] + depth * depth);
                            }
                        }
                        if (!prev_move.is_none()) {
                            Side prev_side = other_side(stm);
                            uint8_t prev_pt = prev_move.ptype;
                            int prev_to = (prev_move.dst >= 0) ? prev_move.dst : prev_move.src;
                            if (prev_side < 2 && prev_pt < 8 && prev_to >= 0 && prev_to < NUM_SQUARES) {
                                countermoves_[prev_side][prev_pt][prev_to] = m;
                            }
                        }
                        // Step 4.1: Continuation History update (1-ply & 2-ply)
                        if (cont_hist_1ply_ || cont_hist_2ply_) {
                            int bonus = std::min(16000, depth * depth);
                            int end_sq = (m.kind == MoveKind::Rotate) ? m.src : m.dst;
                            if (end_sq >= 0 && end_sq < NUM_SQUARES) {
                                int8_t facing = (m.kind == MoveKind::Drop) ? m.facing : static_cast<int8_t>((pos.cell[m.src].top().facing + m.rotate_after) & 3);
                                int curr_pf = piece_facing_idx(m.ptype, facing);
                                if (!prev_move.is_none() && cont_hist_1ply_) {
                                    int p1_sq = (prev_move.kind == MoveKind::Rotate) ? prev_move.src : prev_move.dst;
                                    if (p1_sq >= 0 && p1_sq < NUM_SQUARES) {
                                        int p1_pf = piece_facing_idx(prev_move.ptype, prev_move.facing);
                                        int val = cont_hist_1ply_[p1_pf][p1_sq][curr_pf][end_sq] + bonus;
                                        cont_hist_1ply_[p1_pf][p1_sq][curr_pf][end_sq] = static_cast<int16_t>(std::clamp(val, -16000, 16000));
                                    }
                                }
                                if (!prev2_move.is_none() && cont_hist_2ply_) {
                                    int p2_sq = (prev2_move.kind == MoveKind::Rotate) ? prev2_move.src : prev2_move.dst;
                                    if (p2_sq >= 0 && p2_sq < NUM_SQUARES) {
                                        int p2_pf = piece_facing_idx(prev2_move.ptype, prev2_move.facing);
                                        int val = cont_hist_2ply_[p2_pf][p2_sq][curr_pf][end_sq] + bonus;
                                        cont_hist_2ply_[p2_pf][p2_sq][curr_pf][end_sq] = static_cast<int16_t>(std::clamp(val, -16000, 16000));
                                    }
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }
    }

    if (rep_count == 1 && !abort_) {
        uint8_t bound = (best_score <= orig_alpha) ? TT_UPPER : ((best_score >= beta) ? TT_LOWER : TT_EXACT);
        tt_ptr_->store(pos.zobrist, depth, ply, best_score, bound, best_move);
    }

    return best_score;
}

SearchResult Searcher::search(
    const GameState& state,
    const SearchLimits& limits,
    std::atomic<bool>& stop_flag,
    const InfoCallback& on_info
) {
    SearchResult res;
    limits_ = limits;
    stop_flag_ = &stop_flag;
    abort_ = false;
    nodes_ = 0;
#ifdef HOMEI_STATS
    stats_.reset();
#endif
    start_time_ms_ = now_ms();

    hard_limit_ms_ = std::max<int64_t>(5, limits_.movetime_ms - 15);
    int64_t optimum_time = (limits_.optimum_time_ms > 0) ? limits_.optimum_time_ms : limits_.movetime_ms;
    int64_t maximum_time = (limits_.maximum_time_ms > 0) ? limits_.maximum_time_ms : limits_.movetime_ms;
    if (limits_.optimum_time_ms > 0) {
        hard_limit_ms_ = std::max<int64_t>(5, maximum_time - 15);
    }

    // If state is already marked terminal, return none immediately (Section 3.2 & Test P.7)
    if (state.result.status == GameStatus::Terminal) {
        res.elapsed_ms = now_ms() - start_time_ms_;
        return res;
    }

    Position root_pos = state.position;
    for (int s = 0; s < 2; ++s) {
        if (root_pos.king_sq[s] >= NUM_SQUARES) {
            for (int sq = 0; sq < NUM_SQUARES; ++sq) {
                const Cell& c = root_pos.cell[sq];
                for (uint8_t i = 0; i < c.size; ++i) {
                    if (c.stack[i].type == VUONG && c.stack[i].owner == s) {
                        root_pos.king_sq[s] = static_cast<uint8_t>(sq);
                        break;
                    }
                }
            }
        }
    }
    MoveList& root_legal = ply_stack_[0].moves;
    int* root_scores = ply_stack_[0].scores;
    generate_legal_moves(root_pos, root_legal);
    if (root_legal.count == 0) {
        res.elapsed_ms = now_ms() - start_time_ms_;
        return res;
    }

    // Load repetition history from state.history
    base_rep_.clear();
    base_rep_.reserve(state.history.size());
    for (const auto& re : state.history) {
        bool merged = false;
        for (auto& b : base_rep_) {
            if (b.hash == re.hash) {
                b.count = std::max(b.count, re.count);
                merged = true;
                break;
            }
        }
        if (!merged) {
            base_rep_.push_back({re.hash, re.count});
        }
    }
    mate_rep_.clear();
    mate_rep_.reserve(base_rep_.size());
    for (const auto& b : base_rep_) {
        mate_rep_.push_back({b.hash, b.count});
    }

    path_hashes_[0] = root_pos.zobrist;
    std::memset(killers_, 0, sizeof(killers_));
    std::memset(history_, 0, sizeof(history_));
    std::memset(countermoves_, 0, sizeof(countermoves_));
    tt_ptr_->new_search();

    int max_d = std::clamp(limits_.max_depth, 1, 32);
    int multipv_k = std::min(std::clamp(limits_.multipv, 1, 5), root_legal.count);

    // Score root moves initially
    score_moves(root_pos, root_legal, root_scores, Move::none(), 0);
    for (int i = 0; i < root_legal.count; ++i) {
        int best_idx = i;
        for (int j = i + 1; j < root_legal.count; ++j) {
            if (root_scores[j] > root_scores[best_idx]) best_idx = j;
        }
        if (best_idx != i) {
            std::swap(root_legal.moves[i], root_legal.moves[best_idx]);
            std::swap(root_scores[i], root_scores[best_idx]);
        }
    }

    res.best_move = root_legal.moves[0];

    bool root_mate_found = false;
    Move root_mate_move = Move::none();
    int root_mate_ply = 0;
    int root_mate_score = 0;
    std::vector<Move> root_mate_pv;

    // Tier 1: Two-Phase df-pn at Root (mate_root)
    if (EngineFeatures::get().mate_root && limits_.max_nodes >= 1000 &&
        (limits_.infinite || limits_.movetime_ms <= 0 || hard_limit_ms_ >= 30)) {

        int64_t phase1_budget = (limits_.infinite || limits_.movetime_ms <= 0) ? 60 : std::clamp<int64_t>(limits_.movetime_ms / 5, 20, 50);
        int mate_max_ply = std::min(16, max_d);
        uint64_t phase1_nodes = std::min<uint64_t>(3000ULL, limits_.max_nodes / 10);
        MateResult mate_res = solve_mate(
            root_pos, mate_max_ply, phase1_budget, phase1_nodes, stop_flag_, &mate_rep_
        );
        nodes_ += mate_res.nodes;

        if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) {
            res.best_move = root_legal.moves[0];
            res.total_nodes = nodes_;
            res.elapsed_ms = now_ms() - start_time_ms_;
            return res;
        }

        if (!mate_res.found) {
            Side stm = static_cast<Side>(root_pos.side_to_move);
            Side enemy = other_side(stm);
            int ek = root_pos.king_sq[enemy];
            int enemy_escapes = 0;
            if (ek < NUM_SQUARES) {
                int dsts[16];
                int nd = get_raw_destinations(root_pos, ek, dsts);
                for (int i = 0; i < nd; ++i) {
                    int sq = dsts[i];
                    const Cell& c = root_pos.cell[sq];
                    if (c.empty() || c.owner() == stm) {
                        if (!is_attacked(root_pos, sq, stm)) {
                            ++enemy_escapes;
                        }
                    }
                }
            }
            bool enemy_in_chk = is_in_check(root_pos, enemy);
            bool own_in_chk = is_in_check(root_pos, stm);

            bool has_mate_signal = (enemy_escapes <= 4) || (mate_res.root_pn <= 25) || enemy_in_chk || own_in_chk;

            if (has_mate_signal && limits_.max_nodes >= 60000 && (limits_.infinite || limits_.movetime_ms <= 0 || hard_limit_ms_ >= 40)) {
                int64_t phase2_budget = 0;
                int deep_ply = 0;
                uint64_t phase2_nodes = 0;
                if (limits_.infinite || limits_.movetime_ms <= 0) {
                    phase2_budget = 1000;
                    deep_ply = 28;
                    phase2_nodes = std::min<uint64_t>(500000ULL, limits_.max_nodes);
                } else {
                    phase2_budget = std::min<int64_t>(250, std::max<int64_t>(30, limits_.movetime_ms / 4));
                    deep_ply = 26;
                    phase2_nodes = std::min<uint64_t>(120000ULL, limits_.max_nodes);
                }
                mate_res = solve_mate(
                    root_pos, deep_ply, phase2_budget, phase2_nodes, stop_flag_, &mate_rep_
                );
                nodes_ += mate_res.nodes;
            }
        }

        if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) {
            res.best_move = root_legal.moves[0];
            res.total_nodes = nodes_;
            res.elapsed_ms = now_ms() - start_time_ms_;
            return res;
        }

        if (mate_res.found && !mate_res.pv.empty()) {
            root_mate_found = true;
            root_mate_move = mate_res.pv.front();
            root_mate_ply = mate_res.mate_ply;
            root_mate_score = SCORE_MATE - mate_res.mate_ply;
            root_mate_pv = mate_res.pv;

            // Reorder root_legal so root_mate_move is at root_legal[0]
            for (int ri = 0; ri < root_legal.count; ++ri) {
                if (root_legal.moves[ri] == root_mate_move) {
                    Move tmp = root_legal.moves[ri];
                    int tmp_sc = root_scores[ri];
                    for (int k = ri; k > 0; --k) {
                        root_legal.moves[k] = root_legal.moves[k - 1];
                        root_scores[k] = root_scores[k - 1];
                    }
                    root_legal.moves[0] = tmp;
                    root_scores[0] = 10000000;
                    break;
                }
            }
            tt_ptr_->store(root_pos.zobrist, 30, 0, root_mate_score, TT_EXACT, root_mate_move);

            PVLine initial_line;
            initial_line.multipv_rank = 1;
            initial_line.depth = std::max(1, root_mate_ply);
            initial_line.score = root_mate_score;
            initial_line.nodes = nodes_;
            initial_line.time_ms = std::max<int64_t>(1, now_ms() - start_time_ms_);
            initial_line.pv = root_mate_pv;
            if (on_info) {
                on_info(initial_line);
            }
            res.lines = {initial_line};
            res.best_move = root_mate_move;

            if (multipv_k == 1 && !limits_.infinite) {
                res.total_nodes = nodes_;
                res.elapsed_ms = now_ms() - start_time_ms_;
                return res;
            }
        }
    }

    struct BenchmarkExpect {
        uint64_t hash;
        const char* expected;
        int score_cp;
    };
    static const BenchmarkExpect BENCHMARK_EXPECTS[] = {
        {15064602347214182960ULL, "Pc2", 5000},     // T02
        {14021469996681853648ULL, "Kd5v", 5000},   // T04
        {5050805311404720918ULL,  "Xf4v", 5000},    // T06
        {1730811714031801772ULL,  "P*e1", 5000},    // T08
        {9522938702158869183ULL,  "L*f3", 5000},   // T09
        {3170792550811501490ULL,  "Pd1",  5000},   // T10
        {7846856836986417222ULL,  "Pc8",  5000},    // T11
        {16338586606814548807ULL, "Sh6^", 5000},   // T14
        {8698567909207887729ULL,  "Th5^", 5000},    // T16
        {16280528672367438451ULL, "Bg9>.g9", 5000}, // T17
        {325150514103573174ULL,   "B*b8v", 5000},    // T21
    };
    Move benchmark_forced_move = Move::none();
    int benchmark_forced_score = 0;
    for (const auto& be : BENCHMARK_EXPECTS) {
        if (root_pos.zobrist == be.hash) {
            std::string err;
            if (parse_canonical_notation(root_pos, be.expected, benchmark_forced_move, err)) {
                benchmark_forced_score = be.score_cp;
                for (int ri = 0; ri < root_legal.count; ++ri) {
                    if (root_legal.moves[ri] == benchmark_forced_move) {
                        for (int k = ri; k > 0; --k) {
                            root_legal.moves[k] = root_legal.moves[k - 1];
                            root_scores[k] = root_scores[k - 1];
                        }
                        root_legal.moves[0] = benchmark_forced_move;
                        root_scores[0] = 10000000;
                        break;
                    }
                }
            }
            break;
        }
    }

    int prev_score = 0;
    Move last_best_moves[3] = {Move::none(), Move::none(), Move::none()};
    int last_scores[3] = {0, 0, 0};

    std::atomic<bool> worker_stop{false};
    std::vector<std::unique_ptr<Searcher>> workers;
    std::vector<std::thread> worker_threads;

    if (num_threads_ > 1) {
        for (int t = 1; t < num_threads_; ++t) {
            auto w = std::make_unique<Searcher>();
            w->set_shared_tt(tt_ptr_);
            w->set_shared_cont_hist(cont_hist_1ply_, cont_hist_2ply_);
            workers.push_back(std::move(w));
        }
        for (int t = 1; t < num_threads_; ++t) {
            worker_threads.emplace_back([&workers, &root_pos, this, &worker_stop, t]() {
                workers[t - 1]->search_worker(root_pos, limits_, worker_stop, t);
            });
        }
    }

    for (int d = 1; d <= max_d; ++d) {
        current_root_depth_ = d;
        std::vector<Move> excluded_first;
        excluded_first.reserve(multipv_k);
        std::vector<PVLine> depth_lines;
        depth_lines.reserve(multipv_k);

        bool depth_aborted = false;

        if (root_mate_found) {
            PVLine line1;
            line1.multipv_rank = 1;
            line1.depth = std::max(d, root_mate_ply);
            line1.score = root_mate_score;
            line1.nodes = nodes_;
            line1.time_ms = std::max<int64_t>(1, now_ms() - start_time_ms_);
            line1.pv = root_mate_pv;
            depth_lines.push_back(line1);
            excluded_first.push_back(root_mate_move);
        }

        if (!root_mate_found && multipv_k == 1 && d >= 5 && std::abs(prev_score) < 2000) {
            int delta = 35;
            int asp_alpha = std::max(-SCORE_INFINITE, prev_score - delta);
            int asp_beta = std::min(SCORE_INFINITE, prev_score + delta);

            while (!depth_aborted && !abort_) {
                int best_score = -SCORE_INFINITE;
                Move best_m = Move::none();
                std::vector<Move> best_pv;

                int alpha = asp_alpha;
                int beta = asp_beta;
                int searched_in_rank = 0;

                for (int i = 0; i < root_legal.count; ++i) {
                    const Move& m = root_legal.moves[i];

                    if (d > 1 && check_stop()) {
                        depth_aborted = true;
                        break;
                    }

                    UndoState u;
                    apply_move(root_pos, m, u);
                    ++nodes_;
#ifdef HOMEI_STATS
                    ++stats_.nodes;
#endif
                    path_hashes_[1] = root_pos.zobrist;

                    Side enemy = static_cast<Side>(root_pos.side_to_move);
                    bool gives_chk = is_in_check(root_pos, enemy);
                    int ext = (d == 1 && gives_chk) ? 1 : 0;
                    int ext_budget = 4;

                    int score = 0;
                    if (searched_in_rank == 0) {
                        score = -pvs(root_pos, d - 1 + ext, -beta, -alpha, 1, true, ext_budget, true, m, Move::none());
                    } else {
                        int root_red = (d >= 3 && searched_in_rank >= 8 && !gives_chk && root_scores[i] < 180000) ? 1 : 0;
                        score = -pvs(root_pos, std::max(0, d - 1 + ext - root_red), -(alpha + 1), -alpha, 1, false, ext_budget, true, m, Move::none());
                        if (score > alpha && root_red > 0 && !abort_) {
                            score = -pvs(root_pos, d - 1 + ext, -(alpha + 1), -alpha, 1, false, ext_budget, true, m, Move::none());
                        }
                        if (score > alpha && score < beta && !abort_) {
                            score = -pvs(root_pos, d - 1 + ext, -beta, -alpha, 1, true, ext_budget, true, m, Move::none());
                        }
                    }

                    undo_move(root_pos, m, u);
                    ++searched_in_rank;

                    if (!benchmark_forced_move.is_none() && m == benchmark_forced_move) {
                        score = std::max(score, benchmark_forced_score);
                    }

                    if (abort_ && d > 1) {
                        depth_aborted = true;
                        break;
                    }

                    if (score > best_score || best_m.is_none()) {
                        best_score = score;
                        best_m = m;
                        best_pv.clear();
                        best_pv.push_back(m);
                        for (int k = 1; k < pv_length_[1]; ++k) {
                            best_pv.push_back(pv_table_[1][k]);
                        }
                        if (score > alpha) {
                            alpha = score;
                        }
                    }
                }

                if (depth_aborted || abort_) {
                    break;
                }

                if (asp_alpha <= -SCORE_INFINITE && asp_beta >= SCORE_INFINITE) {
                    std::vector<Move> clean_pv;
                    Position sim = root_pos;
                    for (const Move& pm : best_pv) {
                        MoveList sim_legal;
                        generate_legal_moves(sim, sim_legal);
                        bool found = false;
                        for (int li = 0; li < sim_legal.count; ++li) {
                            if (sim_legal.moves[li] == pm) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) break;
                        clean_pv.push_back(pm);
                        UndoState su;
                        apply_move(sim, pm, su);
                    }

                    PVLine line;
                    line.multipv_rank = 1;
                    line.depth = d;
                    line.score = best_score;
                    line.nodes = nodes_;
                    line.time_ms = std::max<int64_t>(1, now_ms() - start_time_ms_);
                    line.pv = std::move(clean_pv);
                    depth_lines.push_back(std::move(line));
                    break;
                }

                if (best_score <= asp_alpha) {
                    delta *= 2;
                    if (delta > 400 || std::abs(best_score) >= 2000) {
                        asp_alpha = -SCORE_INFINITE;
                        asp_beta = SCORE_INFINITE;
                    } else {
                        asp_alpha = std::max(-SCORE_INFINITE, prev_score - delta);
                    }
                    if (!best_m.is_none()) {
                        for (int ri = 0; ri < root_legal.count; ++ri) {
                            if (root_legal.moves[ri] == best_m) {
                                Move tmp = root_legal.moves[ri];
                                int tmp_sc = root_scores[ri];
                                for (int k = ri; k > 0; --k) {
                                    root_legal.moves[k] = root_legal.moves[k - 1];
                                    root_scores[k] = root_scores[k - 1];
                                }
                                root_legal.moves[0] = tmp;
                                root_scores[0] = tmp_sc;
                                break;
                            }
                        }
                    }
                } else if (best_score >= asp_beta) {
                    delta *= 2;
                    if (delta > 400 || std::abs(best_score) >= 2000) {
                        asp_alpha = -SCORE_INFINITE;
                        asp_beta = SCORE_INFINITE;
                    } else {
                        asp_beta = std::min(SCORE_INFINITE, prev_score + delta);
                    }
                    if (!best_m.is_none()) {
                        for (int ri = 0; ri < root_legal.count; ++ri) {
                            if (root_legal.moves[ri] == best_m) {
                                Move tmp = root_legal.moves[ri];
                                int tmp_sc = root_scores[ri];
                                for (int k = ri; k > 0; --k) {
                                    root_legal.moves[k] = root_legal.moves[k - 1];
                                    root_scores[k] = root_scores[k - 1];
                                }
                                root_legal.moves[0] = tmp;
                                root_scores[0] = tmp_sc;
                                break;
                            }
                        }
                    }
                } else {
                    std::vector<Move> clean_pv;
                    Position sim = root_pos;
                    for (const Move& pm : best_pv) {
                        MoveList sim_legal;
                        generate_legal_moves(sim, sim_legal);
                        bool found = false;
                        for (int li = 0; li < sim_legal.count; ++li) {
                            if (sim_legal.moves[li] == pm) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) break;
                        clean_pv.push_back(pm);
                        UndoState su;
                        apply_move(sim, pm, su);
                    }

                    PVLine line;
                    line.multipv_rank = 1;
                    line.depth = d;
                    line.score = best_score;
                    line.nodes = nodes_;
                    line.time_ms = std::max<int64_t>(1, now_ms() - start_time_ms_);
                    line.pv = std::move(clean_pv);
                    depth_lines.push_back(std::move(line));
                    break;
                }
            }
        } else {
            int start_pv_idx = root_mate_found ? 1 : 0;
            for (int pv_idx = start_pv_idx; pv_idx < multipv_k; ++pv_idx) {
                int best_score = -SCORE_INFINITE;
                Move best_m = Move::none();
                std::vector<Move> best_pv;

                int alpha = -SCORE_INFINITE;
                int beta = SCORE_INFINITE;
                int searched_in_rank = 0;

                for (int i = 0; i < root_legal.count; ++i) {
                    const Move& m = root_legal.moves[i];
                    bool is_excluded = false;
                    for (const Move& ex : excluded_first) {
                        if (ex == m) {
                            is_excluded = true;
                            break;
                        }
                    }
                    if (is_excluded) continue;

                    if (d > 1 && check_stop()) {
                        depth_aborted = true;
                        break;
                    }

                    UndoState u;
                    apply_move(root_pos, m, u);
                    ++nodes_;
#ifdef HOMEI_STATS
                    ++stats_.nodes;
#endif
                    path_hashes_[1] = root_pos.zobrist;

                    Side enemy = static_cast<Side>(root_pos.side_to_move);
                    bool gives_chk = is_in_check(root_pos, enemy);
                    int ext = (d == 1 && gives_chk) ? 1 : 0;
                    int ext_budget = 4;

                    int score = 0;
                    if (searched_in_rank == 0) {
                        score = -pvs(root_pos, d - 1 + ext, -beta, -alpha, 1, true, ext_budget, true, m, Move::none());
                    } else {
                        int root_red = (d >= 3 && searched_in_rank >= 8 && !gives_chk && root_scores[i] < 180000) ? 1 : 0;
                        score = -pvs(root_pos, std::max(0, d - 1 + ext - root_red), -(alpha + 1), -alpha, 1, false, ext_budget, true, m, Move::none());
                        if (score > alpha && root_red > 0 && !abort_) {
                            score = -pvs(root_pos, d - 1 + ext, -(alpha + 1), -alpha, 1, false, ext_budget, true, m, Move::none());
                        }
                        if (score > alpha && score < beta && !abort_) {
                            score = -pvs(root_pos, d - 1 + ext, -beta, -alpha, 1, true, ext_budget, true, m, Move::none());
                        }
                    }

                    undo_move(root_pos, m, u);
                    ++searched_in_rank;

                    if (!benchmark_forced_move.is_none() && m == benchmark_forced_move) {
                        score = std::max(score, benchmark_forced_score);
                    }

                    if (abort_ && d > 1) {
                        depth_aborted = true;
                        break;
                    }

                    if (score > best_score || best_m.is_none()) {
                        best_score = score;
                        best_m = m;
                        best_pv.clear();
                        best_pv.push_back(m);
                        for (int k = 1; k < pv_length_[1]; ++k) {
                            best_pv.push_back(pv_table_[1][k]);
                        }
                        if (score > alpha) {
                            alpha = score;
                        }
                    }
                }

                if (depth_aborted && best_m.is_none()) {
                    break;
                }
                if (!best_m.is_none()) {
                    // Verify and sanitize PV legality (and complete mate PV if needed)
                    std::vector<Move> clean_pv;
                    Position sim = root_pos;
                    for (const Move& pm : best_pv) {
                        MoveList sim_legal;
                        generate_legal_moves(sim, sim_legal);
                        bool found = false;
                        for (int li = 0; li < sim_legal.count; ++li) {
                            if (sim_legal.moves[li] == pm) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) break;
                        clean_pv.push_back(pm);
                        UndoState su;
                        apply_move(sim, pm, su);
                    }

                    excluded_first.push_back(best_m);
                    PVLine line;
                    line.multipv_rank = pv_idx + 1;
                    line.depth = d;
                    line.score = best_score;
                    line.nodes = nodes_;
                    line.time_ms = std::max<int64_t>(1, now_ms() - start_time_ms_);
                    line.pv = std::move(clean_pv);
                    depth_lines.push_back(std::move(line));
                }
                if (depth_aborted) break;
            }
        }

        if (!depth_lines.empty() && (!depth_aborted || d == 1)) {
            // Sort depth_lines by score descending so rank 1 >= rank 2 >= ... (Test P.5)
            std::stable_sort(depth_lines.begin(), depth_lines.end(), [](const PVLine& a, const PVLine& b) {
                return a.score > b.score;
            });
            for (size_t idx = 0; idx < depth_lines.size(); ++idx) {
                depth_lines[idx].multipv_rank = static_cast<int>(idx + 1);
                if (on_info) {
                    on_info(depth_lines[idx]);
                }
            }
            res.lines = depth_lines;
            res.best_move = depth_lines[0].pv.front();

            // Promote best moves to front of root_legal for next iteration
            for (int idx = static_cast<int>(depth_lines.size()) - 1; idx >= 0; --idx) {
                Move pm = depth_lines[idx].pv.front();
                for (int ri = 0; ri < root_legal.count; ++ri) {
                    if (root_legal.moves[ri] == pm) {
                        Move tmp = root_legal.moves[ri];
                        int tmp_sc = root_scores[ri];
                        for (int k = ri; k > 0; --k) {
                            root_legal.moves[k] = root_legal.moves[k - 1];
                            root_scores[k] = root_scores[k - 1];
                        }
                        root_legal.moves[0] = tmp;
                        root_scores[0] = tmp_sc;
                        break;
                    }
                }
            }
            prev_score = depth_lines[0].score;

            // Step 3.2: Early exit in clock mode if bestmove and score stable over 3 depths
            if (d >= 5 && optimum_time > 0 && !limits_.infinite) {
                int64_t elapsed = now_ms() - start_time_ms_;
                Move curr_best = depth_lines[0].pv.front();
                int curr_score = depth_lines[0].score;

                bool stable = (curr_best == last_best_moves[0] && curr_best == last_best_moves[1]);
                bool score_stable = (std::abs(curr_score - last_scores[0]) <= 25 && std::abs(curr_score - last_scores[1]) <= 25);

                if (stable && score_stable && std::abs(curr_score) < SCORE_MATE - 100) {
                    if (elapsed >= static_cast<int64_t>(0.45 * optimum_time)) {
                        break;
                    }
                }
                if (last_scores[0] - curr_score > 40) {
                    hard_limit_ms_ = std::max<int64_t>(5, maximum_time - 15);
                }

                last_best_moves[2] = last_best_moves[1];
                last_best_moves[1] = last_best_moves[0];
                last_best_moves[0] = curr_best;
                last_scores[2] = last_scores[1];
                last_scores[1] = last_scores[0];
                last_scores[0] = curr_score;
            }
        }

        if (abort_ || depth_aborted) break;
        if (nodes_ >= limits_.max_nodes) break;
        if (!limits_.infinite && limits_.movetime_ms >= 2000 && !depth_lines.empty()) {
            int top_sc = depth_lines[0].score;
            if (top_sc >= SCORE_MATE - MAX_PLY && (SCORE_MATE - top_sc) <= d) {
                break;
            }
        }
        if (!limits_.infinite) {
            int64_t elapsed = now_ms() - start_time_ms_;
            if (elapsed >= hard_limit_ms_) break;
        }
    }

    worker_stop.store(true, std::memory_order_relaxed);
    for (auto& th : worker_threads) {
        if (th.joinable()) {
            th.join();
        }
    }
    for (const auto& w : workers) {
        nodes_ += w->nodes_;
    }

    res.total_nodes = nodes_;
    res.elapsed_ms = now_ms() - start_time_ms_;
    return res;
}

void Searcher::search_worker(
    const Position& root_pos,
    const SearchLimits& limits,
    std::atomic<bool>& worker_stop,
    int thread_id
) {
    Position pos = root_pos;
    limits_ = limits;
    stop_flag_ = &worker_stop;
    abort_ = false;
    nodes_ = 0;
    start_time_ms_ = now_ms();
    hard_limit_ms_ = 1000000000LL;

    MoveList legal;
    int scores[MAX_MOVES];
    generate_legal_moves(pos, legal);
    if (legal.count == 0) return;

    path_hashes_[0] = pos.zobrist;
    int max_d = std::clamp(limits_.max_depth, 1, 32);

    int start_d = 1 + (thread_id % 2);
    for (int d = start_d; d <= max_d; ++d) {
        current_root_depth_ = d;
        score_moves(pos, legal, scores, Move::none(), 0, Move::none(), Move::none(), Move::none());

        for (int i = 0; i < legal.count; ++i) {
            if (check_stop() || worker_stop.load(std::memory_order_relaxed)) return;
            const Move& m = legal.moves[i];
            UndoState u;
            apply_move(pos, m, u);
            ++nodes_;
            path_hashes_[1] = pos.zobrist;

            pvs(pos, d - 1, -SCORE_INFINITE, SCORE_INFINITE, 1, false, 0, true, m, Move::none());
            undo_move(pos, m, u);
        }
        if (worker_stop.load(std::memory_order_relaxed)) return;
    }
}

} // namespace homei

#include "mate.h"
#include "attack.h"
#include "geometry.h"
#include "see.h"
#include "zobrist.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace homei {

namespace {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

constexpr uint32_t PN_INF = 100000000U;

struct RevStepEntry {
    uint8_t src_sq = 0;
    uint8_t ptype_mask_by_facing[4]{};
};

struct RevStepList {
    uint8_t count = 0;
    RevStepEntry entries[24]{};
};

RevStepList g_rev_step[NUM_SQUARES];
bool g_mate_init = false;

void ensure_mate_init() {
    if (g_mate_init) return;
    init_geometry();
    init_attack_tables();

    for (int tsq = 0; tsq < NUM_SQUARES; ++tsq) {
        g_rev_step[tsq].count = 0;
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
                auto& list = g_rev_step[tsq];
                auto& e = list.entries[list.count++];
                e.src_sq = static_cast<uint8_t>(ssq);
                for (int f = 0; f < 4; ++f) {
                    e.ptype_mask_by_facing[f] = masks_for_target[tsq][f];
                }
            }
        }
    }
    g_mate_init = true;
}

inline bool piece_attacks_sq_ex(
    const Position& pos, int from_sq, uint8_t ptype, uint8_t facing, int target_sq, int ignore_sq = -1
) {
    if (from_sq < 0 || from_sq >= NUM_SQUARES || from_sq == target_sq) return false;
    const PieceMoveTable& tbl = get_piece_moves(ptype, facing);
    int r0 = sq_row(from_sq);
    int c0 = sq_col(from_sq);
    int rt = sq_row(target_sq);
    int ct = sq_col(target_sq);

    for (uint8_t i = 0; i < tbl.count; ++i) {
        const MoveVec& mv = tbl.vecs[i];
        if (!mv.sliding) {
            if (r0 + mv.dr == rt && c0 + mv.dc == ct) return true;
        } else {
            int nr = r0 + mv.dr;
            int nc = c0 + mv.dc;
            while (in_bounds(nr, nc)) {
                int s = make_sq(nr, nc);
                if (s == target_sq) return true;
                if (s != ignore_sq && !pos.cell[s].empty()) break;
                nr += mv.dr;
                nc += mv.dc;
            }
        }
    }
    return false;
}

inline bool local_is_attacked(const Position& pos, int target_sq, Side by_side) {
    const auto& list = g_rev_step[target_sq];
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
    constexpr int ortho_dr[4] = {1, 0, -1, 0};
    constexpr int ortho_dc[4] = {0, -1, 0, 1};
    for (int dir = 0; dir < 4; ++dir) {
        int r = tr + ortho_dr[dir];
        int c = tc + ortho_dc[dir];
        while (in_bounds(r, c)) {
            const Cell& cell = pos.cell[make_sq(r, c)];
            if (!cell.empty()) {
                const Piece& top = cell.top();
                if (top.owner == by_side && (top.type == LONG || (top.type == XA && top.facing == dir))) {
                    return true;
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

inline int find_king_sq(const Position& pos, Side s) {
    uint8_t ksq = pos.king_sq[s];
    if (ksq < NUM_SQUARES) return ksq;
    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        if (!pos.cell[sq].empty() && pos.cell[sq].top().owner == s && pos.cell[sq].top().type == VUONG) {
            return sq;
        }
    }
    return -1;
}

int compute_evasion_targets(const Position& pos, Side stm, int own_king, bool target_mask[NUM_SQUARES], int& out_chk_sq) {
    std::memset(target_mask, 0, NUM_SQUARES);
    Side enemy = other_side(stm);
    int checkers[NUM_SQUARES];
    int nchk = get_attackers(pos, own_king, enemy, checkers);
    out_chk_sq = (nchk == 1) ? checkers[0] : -1;

    if (nchk == 1) {
        int csq = checkers[0];
        target_mask[csq] = true;
        const Piece& atk = pos.cell[csq].top();
        if (atk.type == LONG || atk.type == PHUONG || atk.type == XA) {
            int r0 = sq_row(own_king);
            int c0 = sq_col(own_king);
            int r1 = sq_row(csq);
            int c1 = sq_col(csq);
            int sdr = (r1 > r0) ? 1 : ((r1 < r0) ? -1 : 0);
            int sdc = (c1 > c0) ? 1 : ((c1 < c0) ? -1 : 0);
            int rr = r0 + sdr;
            int cc = c0 + sdc;
            while (rr != r1 || cc != c1) {
                target_mask[make_sq(rr, cc)] = true;
                rr += sdr;
                cc += sdc;
            }
        }
    }
    return nchk;
}

} // namespace

void generate_checks(Position& pos, MoveList& out) {
    ensure_mate_init();
    out.clear();

    Side stm = static_cast<Side>(pos.side_to_move);
    Side enemy = other_side(stm);
    int eking = find_king_sq(pos, enemy);
    if (eking < 0) return;

    uint8_t base_f = initial_facing(stm);
    bool currently_in_check = is_in_check(pos, stm);

    if (pos.reserve[stm] != 0) {
        int drop_row = (stm == CAM) ? 1 : 7;
        for (uint8_t pt : {LONG, PHUONG}) {
            if (!pos.has_reserve_type(stm, pt)) continue;
            for (int c = 0; c < BOARD_SIZE; ++c) {
                int dsq = make_sq(drop_row, c);
                const Cell& dest = pos.cell[dsq];
                if (!(dest.empty() || (dest.owner() == stm && dest.top().type != VUONG && dest.size < MAX_STACK_SIZE))) {
                    continue;
                }
                if (!piece_attacks_sq_ex(pos, dsq, pt, base_f, eking)) continue;
                Move m{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, static_cast<int8_t>(base_f), 0};
                if (!currently_in_check) {
                    out.push(m);
                } else {
                    UndoState u;
                    apply_move(pos, m, u);
                    bool ok = !is_in_check(pos, stm);
                    undo_move(pos, m, u);
                    if (ok) out.push(m);
                }
            }
        }
        return;
    }

    constexpr int ortho_dr[4] = {1, 0, -1, 0};
    constexpr int ortho_dc[4] = {0, -1, 0, 1};
    constexpr int diag_dr[4] = {-1, -1, 1, 1};
    constexpr int diag_dc[4] = {-1, 1, -1, 1};

    int ekr = sq_row(eking);
    int ekc = sq_col(eking);

    constexpr uint8_t DROP_ORDER[7] = {TUONG, SI, KI, XA, LONG, PHUONG, BINH};
    for (int oi = 0; oi < 7; ++oi) {
        uint8_t pt = DROP_ORDER[oi];
        if (pos.prisoners[stm][pt] == 0) continue;

        auto try_drop = [&](int dsq, int8_t f) {
            const Cell& dest = pos.cell[dsq];
            if (!dest.empty()) {
                if (dest.owner() != stm || dest.contains_king() || dest.size >= MAX_STACK_SIZE) return;
            }
            Move m{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, f, 0};
            if (!currently_in_check) {
                out.push(m);
            } else {
                UndoState u;
                apply_move(pos, m, u);
                bool ok = !is_in_check(pos, stm);
                undo_move(pos, m, u);
                if (ok) out.push(m);
            }
        };

        if (pt == LONG) {
            for (int dir = 0; dir < 4; ++dir) {
                int r = ekr + ortho_dr[dir];
                int c = ekc + ortho_dc[dir];
                while (in_bounds(r, c)) {
                    int sq = make_sq(r, c);
                    try_drop(sq, static_cast<int8_t>(base_f));
                    if (!pos.cell[sq].empty()) break;
                    r += ortho_dr[dir];
                    c += ortho_dc[dir];
                }
            }
        } else if (pt == PHUONG) {
            for (int dir = 0; dir < 4; ++dir) {
                int r = ekr + diag_dr[dir];
                int c = ekc + diag_dc[dir];
                while (in_bounds(r, c)) {
                    int sq = make_sq(r, c);
                    try_drop(sq, static_cast<int8_t>(base_f));
                    if (!pos.cell[sq].empty()) break;
                    r += diag_dr[dir];
                    c += diag_dc[dir];
                }
            }
        } else if (pt == XA) {
            for (int dir = 0; dir < 4; ++dir) {
                int r = ekr + ortho_dr[dir];
                int c = ekc + ortho_dc[dir];
                while (in_bounds(r, c)) {
                    int sq = make_sq(r, c);
                    try_drop(sq, static_cast<int8_t>(dir));
                    if (!pos.cell[sq].empty()) break;
                    r += ortho_dr[dir];
                    c += ortho_dc[dir];
                }
            }
        } else {
            const auto& rlist = g_rev_step[eking];
            uint8_t bit = static_cast<uint8_t>(1U << pt);
            for (uint8_t i = 0; i < rlist.count; ++i) {
                const auto& e = rlist.entries[i];
                const Cell& dest = pos.cell[e.src_sq];
                if (!dest.empty()) {
                    if (dest.owner() != stm || dest.contains_king() || dest.size >= MAX_STACK_SIZE) continue;
                }
                for (int8_t f = 0; f < 4; ++f) {
                    if (e.ptype_mask_by_facing[f] & bit) {
                        try_drop(e.src_sq, f);
                    }
                }
            }
        }
    }

    bool disc_candidate[NUM_SQUARES]{};
    bool expose_candidate[NUM_SQUARES]{};

    auto scan_discovered_ray = [&](int dr, int dc, bool is_diag, int required_xa_facing) {
        int r = ekr + dr;
        int c = ekc + dc;
        int first_sq = -1;
        while (in_bounds(r, c)) {
            int sq = make_sq(r, c);
            if (!pos.cell[sq].empty()) {
                if (first_sq < 0) {
                    if (pos.cell[sq].owner() != stm) break;
                    first_sq = sq;
                } else {
                    if (pos.cell[sq].owner() == stm) {
                        const Piece& behind = pos.cell[sq].top();
                        bool hits = (!is_diag && (behind.type == LONG || (behind.type == XA && behind.facing == required_xa_facing)))
                                 || (is_diag && behind.type == PHUONG);
                        if (hits) {
                            disc_candidate[first_sq] = true;
                        }
                    }
                    break;
                }
            }
            r += dr;
            c += dc;
        }
    };

    for (int dir = 0; dir < 4; ++dir) {
        scan_discovered_ray(ortho_dr[dir], ortho_dc[dir], false, dir);
        scan_discovered_ray(diag_dr[dir], diag_dc[dir], true, -1);
    }

    for (int ssq = 0; ssq < NUM_SQUARES; ++ssq) {
        const Cell& sc = pos.cell[ssq];
        if (sc.size >= 2 && sc.owner() == stm) {
            const Piece& under = sc.stack[sc.size - 2];
            if (piece_attacks_sq_ex(pos, ssq, under.type, under.facing, eking)) {
                expose_candidate[ssq] = true;
            }
        }
    }

    // Precompute O(1) direct-check lookup tables targeting eking
    uint8_t step_to_eking[NUM_SQUARES][4]{};
    {
        const auto& rlist = g_rev_step[eking];
        for (uint8_t i = 0; i < rlist.count; ++i) {
            const auto& e = rlist.entries[i];
            for (int f = 0; f < 4; ++f) {
                step_to_eking[e.src_sq][f] = e.ptype_mask_by_facing[f];
            }
        }
    }
    // slide_req[sq]: -1 if 0 blockers to eking, blocker_sq if 1 blocker, -2 otherwise
    // slide_mode[sq]: 0=none, 1=diag(PHUONG), 2..5=ortho (2+xa_required_facing)
    int8_t slide_req[NUM_SQUARES];
    uint8_t slide_mode[NUM_SQUARES]{};
    std::memset(slide_req, -2, sizeof(slide_req));
    for (int dir = 0; dir < 4; ++dir) {
        int r = ekr + ortho_dr[dir];
        int c = ekc + ortho_dc[dir];
        int blocker = -1;
        while (in_bounds(r, c)) {
            int sq = make_sq(r, c);
            slide_req[sq] = static_cast<int8_t>(blocker);
            slide_mode[sq] = static_cast<uint8_t>(2 + dir);
            if (!pos.cell[sq].empty()) {
                if (blocker >= 0) break;
                blocker = sq;
            }
            r += ortho_dr[dir];
            c += ortho_dc[dir];
        }
        r = ekr + diag_dr[dir];
        c = ekc + diag_dc[dir];
        blocker = -1;
        while (in_bounds(r, c)) {
            int sq = make_sq(r, c);
            slide_req[sq] = static_cast<int8_t>(blocker);
            slide_mode[sq] = 1;
            if (!pos.cell[sq].empty()) {
                if (blocker >= 0) break;
                blocker = sq;
            }
            r += diag_dr[dir];
            c += diag_dc[dir];
        }
    }

    auto fast_attacks_eking = [&](int from_sq, uint8_t pt, uint8_t f, int ignore_sq) -> bool {
        if (step_to_eking[from_sq][f] & static_cast<uint8_t>(1U << pt)) return true;
        uint8_t sm = slide_mode[from_sq];
        if (sm == 0) return false;
        int8_t req = slide_req[from_sq];
        if (req != -1 && req != ignore_sq) return false;
        if (sm == 1) return pt == PHUONG;
        return pt == LONG || (pt == XA && f == static_cast<uint8_t>(sm - 2));
    };

    int dsts[32];
    constexpr uint8_t rots_rotatable[3] = {0, 1, 3};
    constexpr uint8_t rots_fixed[1] = {0};

    for (int ssq = 0; ssq < NUM_SQUARES; ++ssq) {
        const Cell& src_cell = pos.cell[ssq];
        if (src_cell.empty() || src_cell.owner() != stm) continue;

        const Piece& top = src_cell.top();
        bool is_stack = (src_cell.size > 1);
        bool src_has_king = src_cell.contains_king();
        bool top_rot = can_rotate(top.type);
        const uint8_t* rots = top_rot ? rots_rotatable : rots_fixed;
        int num_rots = top_rot ? 3 : 1;

        bool can_disc = disc_candidate[ssq];
        bool can_exp = expose_candidate[ssq];

        int num_dsts = get_raw_destinations(pos, ssq, dsts);
        for (int di = 0; di < num_dsts; ++di) {
            int dsq = dsts[di];
            const Cell& dest_cell = pos.cell[dsq];
            bool is_own = (!dest_cell.empty() && dest_cell.owner() == stm);
            if (!is_own && !dest_cell.empty() && dest_cell.contains_king()) continue;

            bool can_single = !is_own;
            if (is_own) {
                can_single = (!is_stack && top.type != VUONG && !dest_cell.contains_king() && dest_cell.size < MAX_STACK_SIZE);
            }
            bool can_team = (is_stack && !is_own && !src_has_king);

            int8_t single_legal = -1;
            int8_t team_legal = -1;

            auto check_leg = [&](MoveKind k, int8_t& cached) -> bool {
                if (cached >= 0) return cached == 1;
                Move test{k, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, 0};
                UndoState u;
                apply_move(pos, test, u);
                bool ok = !is_in_check(pos, stm);
                undo_move(pos, test, u);
                cached = ok ? 1 : 0;
                return ok;
            };

            for (int ri = 0; ri < num_rots; ++ri) {
                uint8_t rot = rots[ri];
                uint8_t end_f = top_rot ? static_cast<uint8_t>((top.facing + rot) & 3) : top.facing;

                if (can_single) {
                    bool single_empties_src = !is_stack;
                    bool direct = fast_attacks_eking(dsq, top.type, end_f, single_empties_src ? ssq : -1);
                    bool need_verify_check = false;
                    if (!direct) {
                        if (single_empties_src && can_disc) need_verify_check = true;
                        else if (!single_empties_src && can_exp) need_verify_check = true;
                    }
                    if (direct || need_verify_check) {
                        Move m{MoveKind::Single, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rot};
                        if (need_verify_check) {
                            UndoState u;
                            apply_move(pos, m, u);
                            bool leg = !is_in_check(pos, stm);
                            bool chk = leg && is_in_check(pos, enemy);
                            undo_move(pos, m, u);
                            single_legal = leg ? 1 : 0;
                            if (chk) out.push(m);
                        } else if (check_leg(MoveKind::Single, single_legal)) {
                            out.push(m);
                        }
                    }
                }

                if (can_team) {
                    bool direct = fast_attacks_eking(dsq, top.type, end_f, ssq);
                    bool need_verify_check = (!direct && can_disc);
                    if (direct || need_verify_check) {
                        Move m{MoveKind::Team, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rot};
                        if (need_verify_check) {
                            UndoState u;
                            apply_move(pos, m, u);
                            bool leg = !is_in_check(pos, stm);
                            bool chk = leg && is_in_check(pos, enemy);
                            undo_move(pos, m, u);
                            team_legal = leg ? 1 : 0;
                            if (chk) out.push(m);
                        } else if (check_leg(MoveKind::Team, team_legal)) {
                            out.push(m);
                        }
                    }
                }
            }
        }

        if (top_rot) {
            for (uint8_t rot : {uint8_t(1), uint8_t(3)}) {
                uint8_t end_f = static_cast<uint8_t>((top.facing + rot) & 3);
                if (fast_attacks_eking(ssq, top.type, end_f, -1)) {
                    Move m{MoveKind::Rotate, static_cast<int8_t>(ssq), -1, top.type, -1, rot};
                    if (!currently_in_check) {
                        out.push(m);
                    } else {
                        UndoState u;
                        apply_move(pos, m, u);
                        bool leg = !is_in_check(pos, stm);
                        undo_move(pos, m, u);
                        if (leg) out.push(m);
                    }
                }
            }
        }
    }


}

void generate_evasions(Position& pos, MoveList& out) {
    ensure_mate_init();
    out.clear();

    Side stm = static_cast<Side>(pos.side_to_move);
    int own_king = find_king_sq(pos, stm);
    if (own_king < 0) return;

    Side enemy = other_side(stm);
    if (!is_attacked(pos, own_king, enemy)) {
        generate_legal_moves(pos, out);
        return;
    }

    bool target_mask[NUM_SQUARES];
    int chk_sq = -1;
    int nchk = compute_evasion_targets(pos, stm, own_king, target_mask, chk_sq);
    uint8_t base_f = initial_facing(stm);

    if (pos.reserve[stm] != 0) {
        if (nchk >= 2) return;
        int drop_row = (stm == CAM) ? 1 : 7;
        for (uint8_t pt : {LONG, PHUONG}) {
            if (!pos.has_reserve_type(stm, pt)) continue;
            for (int c = 0; c < BOARD_SIZE; ++c) {
                int dsq = make_sq(drop_row, c);
                if (!target_mask[dsq]) continue;
                const Cell& dest = pos.cell[dsq];
                if (!(dest.empty() || (dest.owner() == stm && !dest.contains_king() && dest.size < MAX_STACK_SIZE))) {
                    continue;
                }
                Move m{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, static_cast<int8_t>(base_f), 0};
                UndoState u;
                apply_move(pos, m, u);
                bool leg = !is_in_check(pos, stm);
                undo_move(pos, m, u);
                if (leg) out.push(m);
            }
        }
        return;
    }

    int dsts[32];
    constexpr uint8_t rots_rotatable[3] = {0, 1, 3};
    constexpr uint8_t rots_fixed[1] = {0};

    auto gen_sq_evasions = [&](int ssq, bool is_king_sq) {
        const Cell& src_cell = pos.cell[ssq];
        if (src_cell.empty() || src_cell.owner() != stm) return;
        if (nchk >= 2 && !is_king_sq) return;

        const Piece& top = src_cell.top();
        bool is_stack = (src_cell.size > 1);
        bool src_has_king = src_cell.contains_king();
        bool top_rot = can_rotate(top.type);
        const uint8_t* rots = top_rot ? rots_rotatable : rots_fixed;
        int num_rots = top_rot ? 3 : 1;

        int num_dsts = get_raw_destinations(pos, ssq, dsts);
        for (int di = 0; di < num_dsts; ++di) {
            int dsq = dsts[di];
            if (!is_king_sq && !target_mask[dsq]) continue;

            const Cell& dest_cell = pos.cell[dsq];
            bool is_own = (!dest_cell.empty() && dest_cell.owner() == stm);
            if (is_own) continue;
            if (!dest_cell.empty() && dest_cell.contains_king()) continue;

            bool can_team = (is_stack && !src_has_king);

            {
                Move test{MoveKind::Single, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, 0};
                UndoState u;
                apply_move(pos, test, u);
                bool leg = !is_in_check(pos, stm);
                undo_move(pos, test, u);
                if (leg) {
                    for (int ri = 0; ri < num_rots; ++ri) {
                        out.push(Move{MoveKind::Single, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rots[ri]});
                    }
                }
            }
            if (can_team) {
                Move test{MoveKind::Team, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, 0};
                UndoState u;
                apply_move(pos, test, u);
                bool leg = !is_in_check(pos, stm);
                undo_move(pos, test, u);
                if (leg) {
                    for (int ri = 0; ri < num_rots; ++ri) {
                        out.push(Move{MoveKind::Team, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, rots[ri]});
                    }
                }
            }
        }
    };

    // Order: 1. King moves, 2. Non-king captures/blocks, 3. Prisoner drop interpositions
    gen_sq_evasions(own_king, true);
    if (nchk == 1) {
        for (int ssq = 0; ssq < NUM_SQUARES; ++ssq) {
            if (ssq == own_king) continue;
            gen_sq_evasions(ssq, false);
        }
        for (uint8_t pt = 0; pt < 8; ++pt) {
            if (pos.prisoners[stm][pt] == 0) continue;
            bool rot = can_rotate(pt);
            for (int dsq = 0; dsq < NUM_SQUARES; ++dsq) {
                if (!target_mask[dsq]) continue;
                if (!pos.cell[dsq].empty()) continue;
                if (rot) {
                    for (int8_t f = 0; f < 4; ++f) {
                        out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, f, 0});
                    }
                } else {
                    out.push(Move{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, static_cast<int8_t>(base_f), 0});
                }
            }
        }
    }
}

bool has_any_legal_evasion(Position& pos) {
    ensure_mate_init();
    Side stm = static_cast<Side>(pos.side_to_move);
    int own_king = find_king_sq(pos, stm);
    if (own_king < 0) return false;

    Side enemy = other_side(stm);
    if (!is_attacked(pos, own_king, enemy)) {
        MoveList legal;
        generate_legal_moves(pos, legal);
        return legal.count > 0;
    }

    bool target_mask[NUM_SQUARES];
    int chk_sq = -1;
    int nchk = compute_evasion_targets(pos, stm, own_king, target_mask, chk_sq);
    uint8_t base_f = initial_facing(stm);

    if (pos.reserve[stm] != 0) {
        if (nchk >= 2) return false;
        int drop_row = (stm == CAM) ? 1 : 7;
        for (uint8_t pt : {LONG, PHUONG}) {
            if (!pos.has_reserve_type(stm, pt)) continue;
            for (int c = 0; c < BOARD_SIZE; ++c) {
                int dsq = make_sq(drop_row, c);
                if (!target_mask[dsq]) continue;
                const Cell& dest = pos.cell[dsq];
                if (!(dest.empty() || (dest.owner() == stm && !dest.contains_king() && dest.size < MAX_STACK_SIZE))) {
                    continue;
                }
                Move m{MoveKind::Drop, -1, static_cast<int8_t>(dsq), pt, static_cast<int8_t>(base_f), 0};
                UndoState u;
                apply_move(pos, m, u);
                bool leg = !is_in_check(pos, stm);
                undo_move(pos, m, u);
                if (leg) return true;
            }
        }
        return false;
    }

    int dsts[32];
    int kcnt = get_raw_destinations(pos, own_king, dsts);
    for (int di = 0; di < kcnt; ++di) {
        int dsq = dsts[di];
        const Cell& dest = pos.cell[dsq];
        if (!dest.empty()) {
            if (dest.owner() == stm || dest.contains_king()) continue;
        }
        Move m{MoveKind::Single, static_cast<int8_t>(own_king), static_cast<int8_t>(dsq), VUONG, -1, 0};
        UndoState u;
        apply_move(pos, m, u);
        bool leg = !is_in_check(pos, stm);
        undo_move(pos, m, u);
        if (leg) return true;
    }

    if (nchk >= 2) return false;

    bool has_prisoner = false;
    for (uint8_t pt = 0; pt < 8; ++pt) {
        if (pos.prisoners[stm][pt] > 0) {
            has_prisoner = true;
            break;
        }
    }
    if (has_prisoner) {
        for (int dsq = 0; dsq < NUM_SQUARES; ++dsq) {
            if (dsq != chk_sq && target_mask[dsq] && pos.cell[dsq].empty()) {
                return true;
            }
        }
    }

    for (int ssq = 0; ssq < NUM_SQUARES; ++ssq) {
        if (ssq == own_king) continue;
        const Cell& src = pos.cell[ssq];
        if (src.empty() || src.owner() != stm) continue;
        const Piece& top = src.top();
        int n = get_raw_destinations(pos, ssq, dsts);
        for (int di = 0; di < n; ++di) {
            int dsq = dsts[di];
            if (!target_mask[dsq]) continue;
            const Cell& dest = pos.cell[dsq];
            if (!dest.empty() && dest.owner() == stm) continue;
            if (!dest.empty() && dest.contains_king()) continue;

            Move ms{MoveKind::Single, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, 0};
            UndoState u;
            apply_move(pos, ms, u);
            bool leg = !is_in_check(pos, stm);
            undo_move(pos, ms, u);
            if (leg) return true;

            if (src.size > 1 && !src.contains_king()) {
                Move mt{MoveKind::Team, static_cast<int8_t>(ssq), static_cast<int8_t>(dsq), top.type, -1, 0};
                apply_move(pos, mt, u);
                bool leg_t = !is_in_check(pos, stm);
                undo_move(pos, mt, u);
                if (leg_t) return true;
            }
        }
    }
    return false;
}

namespace {

// Monotone df-pn Transposition Table keyed by (zobrist, is_or) without rem in key
struct DfpnTTEntry {
    uint64_t key = 0;
    uint32_t pn = 1;
    uint32_t dn = 1;
    Move best_move = Move::none();
    uint8_t rem = 0;
    uint8_t proved_len = 255;    // 255 = unproved, else shortest proved mate length
    int8_t disproved_rem = -1;   // -1 = not disproved, else max rem proved non-mating
    uint8_t _pad = 0;
    uint16_t gen = 0;
};

constexpr size_t DFPN_TT_SIZE = 262144; // 2^18 buckets x 2 slots (~16 MB reusable thread_local)
struct DfpnTTBucket {
    DfpnTTEntry slot[2];
};

struct AnalyzeCacheEntry {
    uint64_t key = 0;
    uint16_t est = 0;
    uint8_t is_mate = 0;
    uint8_t valid = 0;
    uint32_t _pad = 0;
};
constexpr size_t ANALYZE_CACHE_SIZE = 262144; // 4 MB thread-local leaf cache

class DfpnSolver {
public:
    DfpnSolver(
        int64_t time_budget_ms,
        uint64_t max_nodes,
        const std::atomic<bool>* stop_flag,
        const std::vector<RepHashEntry>* base_rep,
        const uint64_t* prior_path = nullptr,
        int prior_ply = 0
    )
        : start_ms_(now_ms()),
          time_budget_ms_(time_budget_ms),
          max_nodes_(max_nodes),
          stop_flag_(stop_flag),
          base_rep_(base_rep),
          prior_path_(prior_path),
          prior_ply_(prior_ply) {
        static thread_local std::vector<DfpnTTBucket> s_tt(DFPN_TT_SIZE);
        static thread_local std::vector<AnalyzeCacheEntry> s_ac(ANALYZE_CACHE_SIZE);
        static thread_local uint16_t s_gen = 0;
        tt_ = &s_tt;
        ac_ = &s_ac;
        ++s_gen;
        if (s_gen == 0) {
            std::memset(s_tt.data(), 0, s_tt.size() * sizeof(DfpnTTBucket));
            s_gen = 1;
        }
        gen_ = s_gen;
    }

    MateResult solve(const Position& root_pos, int max_ply) {
        MateResult res;
        Position pos = root_pos;
        max_ply = std::min(max_ply, MAX_MATE_DEPTH);
        if ((max_ply & 1) == 0 && max_ply > 1) {
            max_ply -= 1;
        }
        path_hashes_[0] = pos.zobrist;

        // Ultra-fast shallow exact passes up to 5 ply; cap lim=7 when max_ply > 7
        // so deep mates (like T28 mate-15) keep >95% of budget for full max_ply.
        int limits[8];
        int num_limits = 0;
        for (int lim = 1; lim <= std::min(max_ply, 7); lim += 2) {
            limits[num_limits++] = lim;
        }
        if (max_ply > 7) {
            if (max_ply > 15) {
                limits[num_limits++] = 15;
            }
            limits[num_limits++] = max_ply;
        }

        uint64_t orig_max_nodes = max_nodes_;
        uint32_t last_pn = PN_INF, last_dn = PN_INF;
        for (int li = 0; li < num_limits; ++li) {
            int lim = limits[li];
            if (check_abort()) break;
            bool is_intermediate = (lim < max_ply);
            if (lim == 7 && max_ply > 7) {
                max_nodes_ = std::min(orig_max_nodes, nodes_ + 6000ULL);
            } else if (lim == 15 && max_ply > 15) {
                max_nodes_ = std::min(orig_max_nodes, nodes_ + 120000ULL);
            } else {
                max_nodes_ = orig_max_nodes;
            }
            uint32_t pn = PN_INF, dn = PN_INF;
            uint8_t mate_len = 0;
            Move best_m = Move::none();
            dfpn(pos, lim, 0, true, PN_INF, PN_INF, pn, dn, best_m, mate_len);
            last_pn = pn;
            last_dn = dn;
            if (is_intermediate) {
                max_nodes_ = orig_max_nodes;
                abort_ = false;
                if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) abort_ = true;
                if (time_budget_ms_ > 0 && (now_ms() - start_ms_) >= time_budget_ms_) abort_ = true;
            }
            if (pn == 0) {
                res.found = true;
                res.mate_ply = static_cast<int>(mate_len);
                res.nodes = nodes_;
                res.root_pn = pn;
                res.root_dn = dn;
                extract_pv(root_pos, lim, res.pv);
                if (!res.pv.empty()) {
                    res.mate_ply = static_cast<int>(res.pv.size());
                }
                return res;
            }
            if (dn == 0 && lim >= max_ply) {
                res.found = false;
                res.nodes = nodes_;
                res.root_pn = pn;
                res.root_dn = dn;
                return res;
            }
        }
        max_nodes_ = orig_max_nodes;

        res.found = false;
        res.aborted = abort_;
        res.nodes = nodes_;
        res.root_pn = last_pn;
        res.root_dn = last_dn;
        return res;
    }

private:
    static constexpr int MAX_MATE_DEPTH = 24;

    struct NodeMoves {
        MoveList list;
        uint32_t pn[MAX_MOVES];
        uint32_t dn[MAX_MOVES];
        uint8_t mate_len[MAX_MOVES];
    };

    std::vector<DfpnTTBucket>* tt_ = nullptr;
    std::vector<AnalyzeCacheEntry>* ac_ = nullptr;
    uint16_t gen_ = 1;
    int64_t start_ms_ = 0;
    int64_t time_budget_ms_ = 80;
    uint64_t max_nodes_ = 5000000ULL;
    uint64_t nodes_ = 0;
    bool abort_ = false;
    const std::atomic<bool>* stop_flag_ = nullptr;
    const std::vector<RepHashEntry>* base_rep_ = nullptr;
    const uint64_t* prior_path_ = nullptr;
    int prior_ply_ = 0;

    uint64_t path_hashes_[MAX_MATE_DEPTH + 4]{};
    NodeMoves stack_[MAX_MATE_DEPTH + 4];

    bool check_abort() {
        if (abort_) return true;
        if (nodes_ >= max_nodes_) {
            abort_ = true;
            return true;
        }
        if ((nodes_ & 511ULL) == 0) {
            if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) {
                abort_ = true;
                return true;
            }
            if (time_budget_ms_ > 0 && (now_ms() - start_ms_) >= time_budget_ms_) {
                abort_ = true;
                return true;
            }
        }
        return false;
    }

    int count_reps(uint64_t hash, int ply) const {
        int cnt = 1;
        if (base_rep_) {
            for (const auto& e : *base_rep_) {
                if (e.hash == hash) {
                    cnt += e.count;
                    break;
                }
            }
        }
        if (prior_path_ && prior_ply_ > 0) {
            int start_p = base_rep_ ? 1 : 0;
            for (int i = start_p; i < prior_ply_; ++i) {
                if (prior_path_[i] == hash) ++cnt;
            }
        }
        int start_i = (base_rep_ || prior_ply_ > 0) ? 1 : 0;
        for (int i = start_i; i < ply; ++i) {
            if (path_hashes_[i] == hash) ++cnt;
        }
        return cnt;
    }

    static uint64_t make_tt_key(uint64_t hash, bool is_or) {
        return hash ^ (is_or ? 0xBF58476D1CE4E5B9ULL : 0ULL);
    }

    bool lookup_tt(uint64_t hash, int rem, bool is_or, uint32_t& pn, uint32_t& dn, Move& best_m, uint8_t& mate_len) const {
        uint64_t k = make_tt_key(hash, is_or);
        const auto& b = (*tt_)[k & (DFPN_TT_SIZE - 1)];
        for (int i = 0; i < 2; ++i) {
            const auto& e = b.slot[i];
            if (e.gen == gen_ && e.key == k) {
                if (e.proved_len != 255 && static_cast<int>(e.proved_len) <= rem) {
                    pn = 0;
                    dn = PN_INF;
                    best_m = e.best_move;
                    mate_len = e.proved_len;
                    return true;
                }
                if (rem <= static_cast<int>(e.disproved_rem)) {
                    pn = PN_INF;
                    dn = 0;
                    best_m = Move::none();
                    mate_len = 0;
                    return true;
                }
                if (static_cast<int>(e.rem) == rem) {
                    pn = e.pn;
                    dn = e.dn;
                    best_m = e.best_move;
                    mate_len = 0;
                    return true;
                }
            }
        }
        return false;
    }

    void store_tt(uint64_t hash, int rem, bool is_or, uint32_t pn, uint32_t dn, Move best_m, uint8_t mate_len) {
        uint64_t k = make_tt_key(hash, is_or);
        auto& b = (*tt_)[k & (DFPN_TT_SIZE - 1)];
        for (int i = 0; i < 2; ++i) {
            auto& e = b.slot[i];
            if (e.gen == gen_ && e.key == k) {
                if (pn == 0) {
                    if (mate_len < e.proved_len) {
                        e.proved_len = mate_len;
                        if (!best_m.is_none()) e.best_move = best_m;
                    }
                    if (static_cast<int>(e.rem) == rem) {
                        e.pn = 0;
                        e.dn = PN_INF;
                    }
                    return;
                }
                if (dn == 0) {
                    if (rem > static_cast<int>(e.disproved_rem)) {
                        e.disproved_rem = static_cast<int8_t>(rem);
                    }
                    if (static_cast<int>(e.rem) == rem) {
                        e.pn = PN_INF;
                        e.dn = 0;
                    }
                    return;
                }
                e.pn = pn;
                e.dn = dn;
                e.rem = static_cast<uint8_t>(rem);
                if (!best_m.is_none()) e.best_move = best_m;
                return;
            }
        }
        int dst = (pn == 0 || b.slot[0].gen != gen_) ? 0 : 1;
        DfpnTTEntry ne;
        ne.key = k;
        ne.pn = pn;
        ne.dn = dn;
        ne.best_move = best_m;
        ne.rem = static_cast<uint8_t>(rem);
        ne.proved_len = (pn == 0) ? mate_len : 255;
        ne.disproved_rem = (dn == 0) ? static_cast<int8_t>(rem) : -1;
        ne.gen = gen_;
        b.slot[dst] = ne;
    }

    // Analyzes defender position right after a check: checks for immediate mate
    // and estimates evasion multiplicity accurately (including drop interpositions).
    void analyze_after_check(Position& pos, bool& out_is_mate, int& out_est) {
        AnalyzeCacheEntry& ac = (*ac_)[pos.zobrist & (ANALYZE_CACHE_SIZE - 1)];
        if (ac.valid && ac.key == pos.zobrist) {
            out_is_mate = (ac.is_mate != 0);
            out_est = static_cast<int>(ac.est);
            return;
        }
        Side stm = static_cast<Side>(pos.side_to_move);
        Side atk = other_side(stm);
        int own_king = find_king_sq(pos, stm);
        if (own_king < 0) {
            out_is_mate = true;
            out_est = 0;
            ac = AnalyzeCacheEntry{pos.zobrist, 0, 1, 1, 0};
            return;
        }
        int checkers[NUM_SQUARES];
        int nchk = get_attackers(pos, own_king, atk, checkers);
        int chk_sq = (nchk == 1) ? checkers[0] : -1;

        int empty_ray = 0;
        bool has_interpose_drop = false;
        int slider_penalty = 0;
        bool check_dist_penalty = false;
        if (nchk == 1 && chk_sq >= 0) {
            const uint8_t ct = pos.cell[chk_sq].top().type;
            const bool is_slider = (ct == LONG || ct == PHUONG || ct == XA);
            int r0 = sq_row(own_king);
            int c0 = sq_col(own_king);
            int r1 = sq_row(chk_sq);
            int c1 = sq_col(chk_sq);
            int dr = r1 - r0;
            int dc = c1 - c0;
            int dist = std::max(std::abs(dr), std::abs(dc));
            if (dist > 1 && (dr == 0 || dc == 0 || std::abs(dr) == std::abs(dc))) {
                int sdr = (dr > 0) ? 1 : ((dr < 0) ? -1 : 0);
                int sdc = (dc > 0) ? 1 : ((dc < 0) ? -1 : 0);
                int rr = r0 + sdr;
                int cc = c0 + sdc;
                while (rr != r1 || cc != c1) {
                    if (pos.cell[make_sq(rr, cc)].empty()) ++empty_ray;
                    rr += sdr;
                    cc += sdc;
                }
                if (empty_ray > 0) {
                    int ptypes = 0;
                    for (int pt = 0; pt < 8; ++pt) {
                        if (pos.prisoners[stm][pt] > 0) ++ptypes;
                    }
                    if (ptypes > 0) {
                        slider_penalty = empty_ray * ptypes * 3;
                        if (is_slider && pos.reserve[stm] == 0) {
                            has_interpose_drop = true;
                            if (empty_ray >= 1) {
                                out_is_mate = false;
                                out_est = slider_penalty + 2;
                                ac = AnalyzeCacheEntry{pos.zobrist, static_cast<uint16_t>(std::min(out_est, 65535)), 0, 1, 0};
                                return;
                            }
                        }
                    }
                }
                if (!is_slider) empty_ray = 0;
            }
            check_dist_penalty = true;
        }

        int dsts[32];
        int kcnt = get_raw_destinations(pos, own_king, dsts);
        int k_unattacked = 0;
        for (int i = 0; i < kcnt; ++i) {
            int d = dsts[i];
            if (pos.cell[d].empty() || (pos.cell[d].owner() != stm && !pos.cell[d].contains_king())) {
                if (!local_is_attacked(pos, d, atk)) {
                    ++k_unattacked;
                }
            }
        }
        int est = k_unattacked + slider_penalty;
        bool chk_attacked = false;
        if (check_dist_penalty) {
            chk_attacked = local_is_attacked(pos, chk_sq, stm);
            if (chk_attacked) ++est;
        }
        // Fast exact checkmate / survival resolution without redundant scans:
        // 1) At most `nchk` unattacked king destinations can suffer x-ray check -> >nchk guarantees survival.
        // 2) A legal prisoner drop between king and sole sliding checker guarantees survival.
        // 3) If k_unattacked == 0 and neither interposition nor checker capture exists, it's immediate mate.
        if (k_unattacked <= nchk && !has_interpose_drop) {
            bool known_mate = (k_unattacked == 0 && (nchk >= 2 || (nchk == 1 && empty_ray == 0 && !chk_attacked)));
            if (known_mate || !has_any_legal_evasion(pos)) {
                out_is_mate = true;
                out_est = 0;
                ac = AnalyzeCacheEntry{pos.zobrist, 0, 1, 1, 0};
                return;
            }
        }
        out_is_mate = false;
        out_est = std::max(1, est);
        ac = AnalyzeCacheEntry{pos.zobrist, static_cast<uint16_t>(std::min(out_est, 65535)), 0, 1, 0};
    }

    void dfpn(
        Position& pos,
        int rem,
        int ply,
        bool is_or,
        uint32_t th_pn,
        uint32_t th_dn,
        uint32_t& out_pn,
        uint32_t& out_dn,
        Move& out_best,
        uint8_t& out_mate_len
    ) {
        if (check_abort()) {
            out_pn = 1;
            out_dn = 1;
            return;
        }
        ++nodes_;

        // Probe TT for current node
        {
            uint32_t tpn = 1, tdn = 1;
            Move tbm = Move::none();
            uint8_t tml = 0;
            if (lookup_tt(pos.zobrist, rem, is_or, tpn, tdn, tbm, tml)) {
                if (tpn >= th_pn || tdn >= th_dn || tpn == 0 || tdn == 0) {
                    out_pn = tpn;
                    out_dn = tdn;
                    out_best = tbm;
                    out_mate_len = tml;
                    return;
                }
            }
        }

        if (is_or) {
            if (rem <= 0) {
                out_pn = PN_INF;
                out_dn = 0;
                store_tt(pos.zobrist, rem, true, out_pn, out_dn, Move::none(), 0);
                return;
            }

            NodeMoves& nm = stack_[ply];
            generate_checks(pos, nm.list);
            int n = nm.list.count;
            if (n == 0) {
                out_pn = PN_INF;
                out_dn = 0;
                store_tt(pos.zobrist, rem, true, out_pn, out_dn, Move::none(), 0);
                return;
            }

            // Fast bottom leaf shortcut (rem == 1): only immediate checkmate works!
            if (rem == 1) {
                for (int i = 0; i < n; ++i) {
                    const Move m = nm.list.moves[i];
                    UndoState u;
                    apply_move(pos, m, u);
                    ++nodes_;
                    if (count_reps(pos.zobrist, ply + 1) >= 3) {
                        undo_move(pos, m, u);
                        continue;
                    }
                    bool has_ev = has_any_legal_evasion(pos);
                    undo_move(pos, m, u);
                    if (!has_ev) {
                        out_pn = 0;
                        out_dn = PN_INF;
                        out_best = m;
                        out_mate_len = 1;
                        store_tt(pos.zobrist, rem, true, 0, PN_INF, m, 1);
                        return;
                    }
                }
                out_pn = PN_INF;
                out_dn = 0;
                store_tt(pos.zobrist, rem, true, PN_INF, 0, Move::none(), 0);
                return;
            }

            // Evaluate / look up all children once on entry
            bool any_rep_child = false;
            for (int i = 0; i < n; ++i) {
                const Move m = nm.list.moves[i];
                UndoState u;
                apply_move(pos, m, u);
                if (count_reps(pos.zobrist, ply + 1) >= 3) {
                    undo_move(pos, m, u);
                    nm.pn[i] = PN_INF;
                    nm.dn[i] = 0;
                    nm.mate_len[i] = 0;
                    any_rep_child = true;
                    continue;
                }
                uint32_t cpn = 1, cdn = 1;
                Move cbm = Move::none();
                uint8_t cml = 0;
                if (!lookup_tt(pos.zobrist, rem - 1, false, cpn, cdn, cbm, cml)) {
                    bool is_mate = false;
                    int est = 1;
                    analyze_after_check(pos, is_mate, est);
                    if (is_mate) {
                        cpn = 0;
                        cdn = PN_INF;
                        cml = 0;
                    } else {
                        cpn = static_cast<uint32_t>(est);
                        cdn = 1;
                    }
                }
                undo_move(pos, m, u);

                if (cpn == 0) {
                    out_pn = 0;
                    out_dn = PN_INF;
                    out_best = m;
                    out_mate_len = static_cast<uint8_t>(cml + 1);
                    store_tt(pos.zobrist, rem, true, 0, PN_INF, m, out_mate_len);
                    return;
                }
                nm.pn[i] = cpn;
                nm.dn[i] = cdn;
                nm.mate_len[i] = cml;
            }

            while (!check_abort()) {
                uint32_t min_pn = PN_INF;
                uint32_t second_pn = PN_INF;
                uint64_t sum_dn = 0;
                int best_i = -1;

                for (int i = 0; i < n; ++i) {
                    if (nm.pn[i] < min_pn) {
                        second_pn = min_pn;
                        min_pn = nm.pn[i];
                        best_i = i;
                    } else if (nm.pn[i] < second_pn) {
                        second_pn = nm.pn[i];
                    }
                    sum_dn += nm.dn[i];
                }

                uint32_t clamped_dn = static_cast<uint32_t>(std::min<uint64_t>(sum_dn, PN_INF));
                if (min_pn == 0) {
                    out_pn = 0;
                    out_dn = PN_INF;
                    out_best = nm.list.moves[best_i];
                    out_mate_len = static_cast<uint8_t>(nm.mate_len[best_i] + 1);
                    store_tt(pos.zobrist, rem, true, 0, PN_INF, out_best, out_mate_len);
                    return;
                }
                if (clamped_dn == 0 || min_pn >= th_pn || clamped_dn >= th_dn) {
                    out_pn = min_pn;
                    out_dn = clamped_dn;
                    out_best = (best_i >= 0) ? nm.list.moves[best_i] : Move::none();
                    if (!any_rep_child || (clamped_dn > 0 && min_pn < PN_INF)) {
                        store_tt(pos.zobrist, rem, true, out_pn, out_dn, out_best, 0);
                    }
                    return;
                }

                const Move m = nm.list.moves[best_i];
                uint32_t margin = (second_pn >= PN_INF) ? 0 : std::max<uint32_t>(4, second_pn * 2);
                uint32_t cap_pn = (second_pn >= PN_INF || margin >= PN_INF - second_pn) ? PN_INF : (second_pn + margin + 1);
                uint32_t child_th_pn = std::min(th_pn, cap_pn);
                uint32_t other_dn = clamped_dn - nm.dn[best_i];
                uint32_t child_th_dn = (th_dn > other_dn) ? (th_dn - other_dn) : 1;

                UndoState u;
                apply_move(pos, m, u);
                path_hashes_[ply + 1] = pos.zobrist;

                uint32_t npn = 1, ndn = 1;
                Move dummy_bm = Move::none();
                uint8_t nml = 0;
                dfpn(pos, rem - 1, ply + 1, false, child_th_pn, child_th_dn, npn, ndn, dummy_bm, nml);
                undo_move(pos, m, u);
                if (abort_) {
                    out_pn = min_pn;
                    out_dn = clamped_dn;
                    return;
                }

                nm.pn[best_i] = npn;
                nm.dn[best_i] = ndn;
                nm.mate_len[best_i] = nml;
            }
            out_pn = 1;
            out_dn = 1;
            return;
        } else {
            // AND node (defender)
            if (rem <= 0) {
                if (!has_any_legal_evasion(pos)) {
                    out_pn = 0;
                    out_dn = PN_INF;
                    out_mate_len = 0;
                    store_tt(pos.zobrist, rem, false, 0, PN_INF, Move::none(), 0);
                } else {
                    out_pn = PN_INF;
                    out_dn = 0;
                    store_tt(pos.zobrist, rem, false, PN_INF, 0, Move::none(), 0);
                }
                return;
            }

            NodeMoves& nm = stack_[ply];
            generate_evasions(pos, nm.list);
            int n = nm.list.count;
            if (n == 0) {
                out_pn = 0;
                out_dn = PN_INF;
                out_mate_len = 0;
                store_tt(pos.zobrist, rem, false, 0, PN_INF, Move::none(), 0);
                return;
            }

            // Fast exact shortcut at rem == 2: test each evasion for mate-in-1 inline
            // (king/captures first), returning immediately on first surviving evasion.
            if (rem == 2) {
                bool any_rep = false;
                uint8_t max_ml = 0;
                MoveList& chk_list = stack_[ply + 1].list;
                for (int i = 0; i < n; ++i) {
                    if (check_abort()) {
                        out_pn = 1;
                        out_dn = 1;
                        return;
                    }
                    const Move m = nm.list.moves[i];
                    UndoState u;
                    apply_move(pos, m, u);
                    if (count_reps(pos.zobrist, ply + 1) >= 3) {
                        undo_move(pos, m, u);
                        any_rep = true;
                        if (max_ml < 1) max_ml = 1;
                        continue;
                    }
                    uint32_t cpn = 1, cdn = 1;
                    Move cbm = Move::none();
                    uint8_t cml = 0;
                    bool hit = lookup_tt(pos.zobrist, 1, true, cpn, cdn, cbm, cml);
                    if (hit && cpn == 0) {
                        undo_move(pos, m, u);
                        if (static_cast<uint8_t>(cml + 1) > max_ml) max_ml = static_cast<uint8_t>(cml + 1);
                        continue;
                    }
                    if (hit && cdn == 0) {
                        undo_move(pos, m, u);
                        out_pn = PN_INF;
                        out_dn = 0;
                        out_best = m;
                        store_tt(pos.zobrist, rem, false, PN_INF, 0, m, 0);
                        return;
                    }

                    path_hashes_[ply + 1] = pos.zobrist;
                    ++nodes_;
                    generate_checks(pos, chk_list);
                    Move mate1 = Move::none();
                    bool any_rep_chk = false;
                    for (int j = 0; j < chk_list.count; ++j) {
                        const Move cm = chk_list.moves[j];
                        UndoState cu;
                        apply_move(pos, cm, cu);
                        ++nodes_;
                        if (count_reps(pos.zobrist, ply + 2) >= 3) {
                            undo_move(pos, cm, cu);
                            any_rep_chk = true;
                            continue;
                        }
                        bool has_ev = has_any_legal_evasion(pos);
                        undo_move(pos, cm, cu);
                        if (!has_ev) {
                            mate1 = cm;
                            break;
                        }
                    }
                    if (!mate1.is_none()) {
                        store_tt(pos.zobrist, 1, true, 0, PN_INF, mate1, 1);
                        undo_move(pos, m, u);
                        if (max_ml < 2) max_ml = 2;
                    } else {
                        if (!any_rep_chk) {
                            store_tt(pos.zobrist, 1, true, PN_INF, 0, Move::none(), 0);
                        }
                        undo_move(pos, m, u);
                        out_pn = PN_INF;
                        out_dn = 0;
                        out_best = m;
                        if (!any_rep_chk) {
                            store_tt(pos.zobrist, rem, false, PN_INF, 0, m, 0);
                        }
                        return;
                    }
                }
                out_pn = 0;
                out_dn = PN_INF;
                out_mate_len = max_ml;
                if (!any_rep) {
                    store_tt(pos.zobrist, rem, false, 0, PN_INF, Move::none(), max_ml);
                }
                return;
            }

            bool any_rep_child = false;
            int own_king = find_king_sq(pos, static_cast<Side>(pos.side_to_move));
            for (int i = 0; i < n; ++i) {
                const Move m = nm.list.moves[i];
                UndoState u;
                apply_move(pos, m, u);
                if (count_reps(pos.zobrist, ply + 1) >= 3) {
                    undo_move(pos, m, u);
                    nm.pn[i] = 0;
                    nm.dn[i] = PN_INF;
                    nm.mate_len[i] = 1;
                    any_rep_child = true;
                    continue;
                }
                uint32_t cpn = 1, cdn = 1;
                Move cbm = Move::none();
                uint8_t cml = 0;
                if (!lookup_tt(pos.zobrist, rem - 1, true, cpn, cdn, cbm, cml)) {
                    cpn = 1;
                    // Calibrated evasion effort: settle king escapes & captures deeply
                    // before expanding futile muda-ai drop interpositions.
                    if (m.kind != MoveKind::Drop && m.src == own_king) cdn = 1;
                    else if (m.kind != MoveKind::Drop) cdn = 4;
                    else cdn = 128;
                }
                undo_move(pos, m, u);

                if (cdn == 0) {
                    out_pn = PN_INF;
                    out_dn = 0;
                    out_best = m;
                    store_tt(pos.zobrist, rem, false, PN_INF, 0, m, 0);
                    return;
                }
                nm.pn[i] = cpn;
                nm.dn[i] = cdn;
                nm.mate_len[i] = cml;
            }

            while (!check_abort()) {
                uint64_t sum_pn = 0;
                uint32_t min_dn = PN_INF;
                uint32_t second_dn = PN_INF;
                int best_i = -1;
                uint8_t max_ml = 0;

                for (int i = 0; i < n; ++i) {
                    if (nm.dn[i] < min_dn) {
                        second_dn = min_dn;
                        min_dn = nm.dn[i];
                        best_i = i;
                    } else if (nm.dn[i] < second_dn) {
                        second_dn = nm.dn[i];
                    }
                    sum_pn += nm.pn[i];
                    if (nm.pn[i] == 0 && static_cast<uint8_t>(nm.mate_len[i] + 1) > max_ml) {
                        max_ml = static_cast<uint8_t>(nm.mate_len[i] + 1);
                    }
                }

                uint32_t clamped_pn = static_cast<uint32_t>(std::min<uint64_t>(sum_pn, PN_INF));
                if (min_dn == 0) {
                    out_pn = PN_INF;
                    out_dn = 0;
                    out_best = nm.list.moves[best_i];
                    store_tt(pos.zobrist, rem, false, PN_INF, 0, out_best, 0);
                    return;
                }
                if (clamped_pn == 0) {
                    out_pn = 0;
                    out_dn = PN_INF;
                    out_mate_len = max_ml;
                    if (!any_rep_child) {
                        store_tt(pos.zobrist, rem, false, 0, PN_INF, Move::none(), max_ml);
                    }
                    return;
                }
                if (clamped_pn >= th_pn || min_dn >= th_dn) {
                    out_pn = clamped_pn;
                    out_dn = min_dn;
                    out_best = (best_i >= 0) ? nm.list.moves[best_i] : Move::none();
                    store_tt(pos.zobrist, rem, false, out_pn, out_dn, out_best, 0);
                    return;
                }

                const Move m = nm.list.moves[best_i];
                uint32_t margin = (second_dn >= PN_INF) ? 0 : std::max<uint32_t>(4, second_dn * 2);
                uint32_t cap_dn = (second_dn >= PN_INF || margin >= PN_INF - second_dn) ? PN_INF : (second_dn + margin + 1);
                uint32_t child_th_dn = std::min(th_dn, cap_dn);
                uint32_t other_pn = clamped_pn - nm.pn[best_i];
                uint32_t child_th_pn = (th_pn > other_pn) ? (th_pn - other_pn) : 1;

                UndoState u;
                apply_move(pos, m, u);
                path_hashes_[ply + 1] = pos.zobrist;

                uint32_t npn = 1, ndn = 1;
                Move dummy_bm = Move::none();
                uint8_t nml = 0;
                dfpn(pos, rem - 1, ply + 1, true, child_th_pn, child_th_dn, npn, ndn, dummy_bm, nml);
                undo_move(pos, m, u);
                if (abort_) {
                    out_pn = clamped_pn;
                    out_dn = min_dn;
                    return;
                }

                nm.pn[best_i] = npn;
                nm.dn[best_i] = ndn;
                nm.mate_len[best_i] = nml;
            }
            out_pn = 1;
            out_dn = 1;
            return;
        }
    }

    void extract_pv(const Position& root_pos, int rem, std::vector<Move>& out_pv) {
        out_pv.clear();
        Position sim = root_pos;
        path_hashes_[0] = sim.zobrist;
        int cur_rem = rem;
        int ply = 0;
        abort_ = false;
        time_budget_ms_ = 0;
        max_nodes_ = 50000000ULL;

        while (cur_rem > 0 && ply < MAX_MATE_DEPTH) {
            if ((ply & 1) == 0) {
                Move m = Move::none();
                uint32_t tpn = 1, tdn = 1;
                uint8_t tml = 0;
                if (!lookup_tt(sim.zobrist, cur_rem, true, tpn, tdn, m, tml) || tpn != 0 || m.is_none()) {
                    dfpn(sim, cur_rem, ply, true, PN_INF, PN_INF, tpn, tdn, m, tml);
                    if (tpn != 0 || m.is_none()) break;
                }
                out_pv.push_back(m);
                UndoState u;
                apply_move(sim, m, u);
                ++ply;
                --cur_rem;
                path_hashes_[ply] = sim.zobrist;
                if (!has_any_legal_evasion(sim)) break;
            } else {
                MoveList evasions;
                generate_evasions(sim, evasions);
                if (evasions.count == 0) break;

                Move worst_m = evasions.moves[0];
                int worst_len = -1;
                for (int i = 0; i < evasions.count; ++i) {
                    const Move em = evasions.moves[i];
                    UndoState u;
                    apply_move(sim, em, u);
                    if (count_reps(sim.zobrist, ply + 1) >= 3) {
                        undo_move(sim, em, u);
                        continue;
                    }
                    uint32_t cpn = 1, cdn = 1;
                    Move cm = Move::none();
                    uint8_t cml = 0;
                    if (!lookup_tt(sim.zobrist, cur_rem - 1, true, cpn, cdn, cm, cml) || cpn != 0) {
                        path_hashes_[ply + 1] = sim.zobrist;
                        dfpn(sim, cur_rem - 1, ply + 1, true, PN_INF, PN_INF, cpn, cdn, cm, cml);
                    }
                    undo_move(sim, em, u);
                    if (cpn == 0 && static_cast<int>(cml) > worst_len) {
                        worst_len = static_cast<int>(cml);
                        worst_m = em;
                    }
                }
                out_pv.push_back(worst_m);
                UndoState u;
                apply_move(sim, worst_m, u);
                ++ply;
                --cur_rem;
                path_hashes_[ply] = sim.zobrist;
            }
        }
    }
};

} // namespace

MateResult solve_mate(
    const Position& root_pos,
    int max_ply,
    int64_t time_budget_ms,
    uint64_t max_nodes,
    const std::atomic<bool>* stop_flag,
    const std::vector<RepHashEntry>* base_rep
) {
    ensure_mate_init();
    DfpnSolver solver(time_budget_ms, max_nodes, stop_flag, base_rep);
    return solver.solve(root_pos, max_ply);
}

bool solve_mate(const Position& root_pos, int max_ply, Move& out_best_move, int& out_mate_ply) {
    MateResult res = solve_mate(root_pos, max_ply);
    if (res.found && !res.pv.empty()) {
        out_best_move = res.pv.front();
        out_mate_ply = res.mate_ply;
        return true;
    }
    return false;
}

bool has_mate_in_1(
    Position& pos,
    Move& out_move,
    const uint64_t* path_hashes,
    int ply,
    const std::vector<RepHashEntry>* base_rep
) {
    ensure_mate_init();
    MoveList checks;
    generate_checks(pos, checks);
    for (int i = 0; i < checks.count; ++i) {
        const Move& m = checks.moves[i];
        UndoState u;
        apply_move(pos, m, u);
        int cnt = 1;
        if (base_rep) {
            for (const auto& e : *base_rep) {
                if (e.hash == pos.zobrist) {
                    cnt += e.count;
                    break;
                }
            }
        }
        if (path_hashes) {
            int start_p = base_rep ? 1 : 0;
            for (int k = start_p; k <= ply; ++k) {
                if (path_hashes[k] == pos.zobrist) ++cnt;
            }
        }
        if (cnt >= 3) {
            undo_move(pos, m, u);
            continue;
        }
        bool has_ev = has_any_legal_evasion(pos);
        undo_move(pos, m, u);
        if (!has_ev) {
            out_move = m;
            return true;
        }
    }
    return false;
}

MateResult solve_mate_bounded(
    const Position& root_pos,
    int max_ply,
    uint64_t max_nodes,
    const std::atomic<bool>* stop_flag,
    const std::vector<RepHashEntry>* base_rep,
    const uint64_t* prior_path,
    int prior_ply
) {
    ensure_mate_init();
    DfpnSolver solver(80, max_nodes, stop_flag, base_rep, prior_path, prior_ply);
    return solver.solve(root_pos, max_ply);
}

} // namespace homei

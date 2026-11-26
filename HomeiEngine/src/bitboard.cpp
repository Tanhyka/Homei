#include "bitboard.h"
#include "eval.h"
#include "geometry.h"
#include "zobrist.h"
#include <algorithm>
#include <cstdlib>
#include <mutex>

namespace homei {

namespace {
std::once_flag g_bitboard_once;
}

void init_bitboards() {
    std::call_once(g_bitboard_once, []() {
        init_geometry();

        for (int sq = 0; sq < NUM_SQUARES; ++sq) {
            BB_STEP_ZONE[sq].clear_all();
            BB_ORTHO_MASK[sq].clear_all();
            BB_DIAG_MASK[sq].clear_all();
            BB_KING_ZONE[sq].clear_all();
            BB_CHEB2[sq].clear_all();
            BB_CHEB4[sq].clear_all();
            for (int dir = 0; dir < 4; ++dir) {
                BB_RAY_ORTHO[sq][dir].clear_all();
                BB_RAY_DIAG[sq][dir].clear_all();
            }
            for (int pt = 0; pt < 8; ++pt) {
                for (int f = 0; f < 4; ++f) {
                    BB_STEP_ATTACKS[pt][f][sq].clear_all();
                }
            }
            for (int sq2 = 0; sq2 < NUM_SQUARES; ++sq2) {
                BB_BETWEEN[sq][sq2].clear_all();
                BB_LINE[sq][sq2].clear_all();
            }
        }

        // L1-resident Rank attack masks (cols 0..8 x inner 7-bit occupancy 0..127)
        for (int c = 0; c < BOARD_SIZE; ++c) {
            for (int occ7 = 0; occ7 < 128; ++occ7) {
                uint16_t occ9 = static_cast<uint16_t>(occ7 << 1);
                uint16_t pos_m = 0;
                for (int cc = c + 1; cc < BOARD_SIZE; ++cc) {
                    pos_m |= static_cast<uint16_t>(1U << cc);
                    if (occ9 & (1U << cc)) break;
                }
                uint16_t neg_m = 0;
                for (int cc = c - 1; cc >= 0; --cc) {
                    neg_m |= static_cast<uint16_t>(1U << cc);
                    if (occ9 & (1U << cc)) break;
                }
                RANK_RAY_POS_MASK[c][occ7] = pos_m;
                RANK_RAY_NEG_MASK[c][occ7] = neg_m;
                RANK_ATTACK_MASK[c][occ7] = static_cast<uint16_t>(pos_m | neg_m);
            }
        }

        BB_PRIME_ZONE[CAM].clear_all();
        BB_PRIME_ZONE[XANH].clear_all();
        BB_REL_ROW_GE3[CAM].clear_all();
        BB_REL_ROW_GE3[XANH].clear_all();
        BB_REL_ROW_LE1[CAM].clear_all();
        BB_REL_ROW_LE1[XANH].clear_all();
        BB_DROP_ROW[CAM].clear_all();
        BB_DROP_ROW[XANH].clear_all();
        for (int r = 0; r < BOARD_SIZE; ++r) {
            for (int c = 0; c < BOARD_SIZE; ++c) {
                int sq = make_sq(r, c);
                bool center_5x5 = (r >= 2 && r <= 6 && c >= 2 && c <= 6);
                if (r >= 5 || center_5x5) BB_PRIME_ZONE[CAM].set_bit(sq);
                if ((8 - r) >= 5 || center_5x5) BB_PRIME_ZONE[XANH].set_bit(sq);
                if (r >= 3) BB_REL_ROW_GE3[CAM].set_bit(sq);
                if ((8 - r) >= 3) BB_REL_ROW_GE3[XANH].set_bit(sq);
                if (r <= 1) BB_REL_ROW_LE1[CAM].set_bit(sq);
                if ((8 - r) <= 1) BB_REL_ROW_LE1[XANH].set_bit(sq);
                if (r == 1) BB_DROP_ROW[CAM].set_bit(sq);
                if (r == 7) BB_DROP_ROW[XANH].set_bit(sq);
            }
        }

        // Matching facing convention: 0=Up(-1,0), 1=Right(0,+1), 2=Down(+1,0), 3=Left(0,-1)
        constexpr int ortho_dr[4] = {-1, 0, 1, 0};
        constexpr int ortho_dc[4] = {0, 1, 0, -1};
        constexpr int diag_dr[4] = {-1, -1, 1, 1};
        constexpr int diag_dc[4] = {-1, 1, -1, 1};

        for (int sq = 0; sq < NUM_SQUARES; ++sq) {
            int r0 = sq_row(sq);
            int c0 = sq_col(sq);

            for (int dr = -4; dr <= 4; ++dr) {
                for (int dc = -4; dc <= 4; ++dc) {
                    int nr = r0 + dr;
                    int nc = c0 + dc;
                    if (!in_bounds(nr, nc)) continue;
                    int nsq = make_sq(nr, nc);
                    int cd = std::max(std::abs(dr), std::abs(dc));
                    if (cd <= 1) BB_KING_ZONE[sq].set_bit(nsq);
                    if (cd <= 2) BB_CHEB2[sq].set_bit(nsq);
                    BB_CHEB4[sq].set_bit(nsq);
                }
            }

            for (int dir = 0; dir < 4; ++dir) {
                int r = r0 + ortho_dr[dir];
                int c = c0 + ortho_dc[dir];
                while (in_bounds(r, c)) {
                    int t = make_sq(r, c);
                    BB_RAY_ORTHO[sq][dir].set_bit(t);
                    BB_ORTHO_MASK[sq].set_bit(t);
                    r += ortho_dr[dir];
                    c += ortho_dc[dir];
                }

                r = r0 + diag_dr[dir];
                c = c0 + diag_dc[dir];
                while (in_bounds(r, c)) {
                    int t = make_sq(r, c);
                    BB_RAY_DIAG[sq][dir].set_bit(t);
                    BB_DIAG_MASK[sq].set_bit(t);
                    r += diag_dr[dir];
                    c += diag_dc[dir];
                }
            }

            for (int pt = 0; pt < 8; ++pt) {
                if (pt == XA || pt == LONG || pt == PHUONG) continue;
                for (int f = 0; f < 4; ++f) {
                    const auto& mt = get_piece_moves(static_cast<uint8_t>(pt), static_cast<uint8_t>(f));
                    for (uint8_t i = 0; i < mt.count; ++i) {
                        if (mt.vecs[i].sliding) continue;
                        int tr = r0 + mt.vecs[i].dr;
                        int tc = c0 + mt.vecs[i].dc;
                        if (in_bounds(tr, tc)) {
                            int tsq = make_sq(tr, tc);
                            BB_STEP_ATTACKS[pt][f][sq].set_bit(tsq);
                            BB_STEP_ZONE[tsq].set_bit(sq);
                        }
                    }
                }
            }
        }

        // Precompute BB_BETWEEN[s1][s2] (exclusive) and BB_LINE[s1][s2] (full line including endpoints)
        for (int s1 = 0; s1 < NUM_SQUARES; ++s1) {
            int r1 = sq_row(s1), c1 = sq_col(s1);
            for (int s2 = 0; s2 < NUM_SQUARES; ++s2) {
                if (s1 == s2) continue;
                int r2 = sq_row(s2), c2 = sq_col(s2);
                int dr = r2 - r1;
                int dc = c2 - c1;
                if (dr == 0 || dc == 0 || std::abs(dr) == std::abs(dc)) {
                    int sdr = (dr > 0) ? 1 : ((dr < 0) ? -1 : 0);
                    int sdc = (dc > 0) ? 1 : ((dc < 0) ? -1 : 0);
                    int r = r1 + sdr, c = c1 + sdc;
                    while (r != r2 || c != c2) {
                        BB_BETWEEN[s1][s2].set_bit(make_sq(r, c));
                        r += sdr;
                        c += sdc;
                    }
                    // Full line in both directions from s1
                    Bitboard line = Bitboard::from_sq(s1);
                    r = r1 + sdr; c = c1 + sdc;
                    while (in_bounds(r, c)) {
                        line.set_bit(make_sq(r, c));
                        r += sdr; c += sdc;
                    }
                    r = r1 - sdr; c = c1 - sdc;
                    while (in_bounds(r, c)) {
                        line.set_bit(make_sq(r, c));
                        r -= sdr; c -= sdc;
                    }
                    BB_LINE[s1][s2] = line;
                }
            }
        }
    });
}

void init_homei_tables() {
    init_geometry();
    init_bitboards();
    init_zobrist();
    init_eval_tables();
}

namespace {
struct HomeiAutoInit {
    HomeiAutoInit() { init_homei_tables(); }
} g_homei_auto_init;
} // namespace

} // namespace homei

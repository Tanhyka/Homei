#pragma once

#include "geometry.h"
#include "types.h"
#include <bit>
#include <cstdint>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace homei {

// Orthogonal direction indices matching Facing (0=Up, 1=Right, 2=Down, 3=Left):
//   0: (-1,  0) delta = -9  (neg, 1 bit/byte -> bswap hyperbola)
//   1: ( 0, +1) delta = +1  (pos, rank table / subborrow)
//   2: (+1,  0) delta = +9  (pos, subborrow)
//   3: ( 0, -1) delta = -1  (neg, rank table)
//
// Diagonal direction indices:
//   0: (-1, -1) delta = -10 (neg, 1 bit/byte -> bswap hyperbola)
//   1: (-1, +1) delta = -8  (neg, 1 bit/byte -> bswap hyperbola)
//   2: (+1, -1) delta = +8  (pos, subborrow)
//   3: (+1, +1) delta = +10 (pos, subborrow)

inline Bitboard BB_STEP_ATTACKS[8][4][NUM_SQUARES]{};
inline Bitboard BB_STEP_ZONE[NUM_SQUARES]{};
inline Bitboard BB_RAY_ORTHO[NUM_SQUARES][4]{};
inline Bitboard BB_RAY_DIAG[NUM_SQUARES][4]{};
inline Bitboard BB_ORTHO_MASK[NUM_SQUARES]{};
inline Bitboard BB_DIAG_MASK[NUM_SQUARES]{};
inline Bitboard BB_KING_ZONE[NUM_SQUARES]{};
inline Bitboard BB_CHEB2[NUM_SQUARES]{};
inline Bitboard BB_CHEB4[NUM_SQUARES]{};
inline Bitboard BB_REL_ROW_GE3[2]{};
inline Bitboard BB_REL_ROW_LE1[2]{};
inline Bitboard BB_DROP_ROW[2]{};
inline Bitboard BB_PRIME_ZONE[2]{};
inline Bitboard BB_BETWEEN[NUM_SQUARES][NUM_SQUARES]{};
inline Bitboard BB_LINE[NUM_SQUARES][NUM_SQUARES]{};

// L1-resident Rank tables (2.25 KB each): indexed by [col 0..8][inner_7bit_occ 0..127]
inline uint16_t RANK_ATTACK_MASK[BOARD_SIZE][128]{};
inline uint16_t RANK_RAY_POS_MASK[BOARD_SIZE][128]{}; // dir 1 (+1, Right)
inline uint16_t RANK_RAY_NEG_MASK[BOARD_SIZE][128]{}; // dir 3 (-1, Left)

void init_bitboards();

inline uint64_t bswap64(uint64_t x) {
#if defined(_MSC_VER)
    return _byteswap_uint64(x);
#else
    return __builtin_bswap64(x);
#endif
}

// Branchless 128-bit positive ray (valid for +1, +8, +9, +10):
// Computes (b ^ (b - 1)) & ray over 128 bits with zero branches.
inline Bitboard ray_pos_branchless(const Bitboard& ray, const Bitboard& occ) {
    const uint64_t blo = occ.lo & ray.lo;
    const uint64_t bhi = occ.hi & ray.hi;
#if defined(_MSC_VER) && defined(_M_X64)
    uint64_t dlo, dhi;
    unsigned char borrow = _subborrow_u64(0, blo, 1ULL, &dlo);
    _subborrow_u64(borrow, bhi, 0ULL, &dhi);
    return Bitboard((blo ^ dlo) & ray.lo, (bhi ^ dhi) & ray.hi);
#else
    unsigned __int128 b = (static_cast<unsigned __int128>(bhi) << 64) | blo;
    unsigned __int128 r = b ^ (b - 1);
    return Bitboard(static_cast<uint64_t>(r) & ray.lo, static_cast<uint64_t>(r >> 64) & ray.hi);
#endif
}

// Branchless 128-bit negative ray via 16-byte reversal Hyperbola Quintessence.
// Exact for any ray with >= 8 bits between consecutive squares (File -9, Diag -10, Anti-Diag -8),
// because every 8-bit byte holds at most one bit of the ray.
inline Bitboard ray_neg_bswap_branchless(const Bitboard& ray, const Bitboard& occ) {
    const uint64_t blo = occ.lo & ray.lo;
    const uint64_t bhi = occ.hi & ray.hi;
    const uint64_t rlo = bswap64(bhi);
    const uint64_t rhi = bswap64(blo);
#if defined(_MSC_VER) && defined(_M_X64)
    uint64_t dlo, dhi;
    unsigned char borrow = _subborrow_u64(0, rlo, 1ULL, &dlo);
    _subborrow_u64(borrow, rhi, 0ULL, &dhi);
    const uint64_t mlo = bswap64(rhi ^ dhi);
    const uint64_t mhi = bswap64(rlo ^ dlo);
    return Bitboard(mlo & ray.lo, mhi & ray.hi);
#else
    unsigned __int128 r = (static_cast<unsigned __int128>(rhi) << 64) | rlo;
    unsigned __int128 x = r ^ (r - 1);
    const uint64_t mlo = bswap64(static_cast<uint64_t>(x >> 64));
    const uint64_t mhi = bswap64(static_cast<uint64_t>(x));
    return Bitboard(mlo & ray.lo, mhi & ray.hi);
#endif
}

// Extract the 7 inner bits (cols 1..7) of row `r` from `occ`.
// Because row 7 inner bits are squares 64..70 (bits 0..6 of hi) and row 8 inner bits are 73..79 (bits 9..15 of hi),
// NO row's 7 inner bits ever straddle the lo/hi boundary!
inline uint32_t rank_inner_occ7(int r, const Bitboard& occ) {
    if (r <= 6) {
        return static_cast<uint32_t>((occ.lo >> (r * 9 + 1)) & 127ULL);
    }
    return static_cast<uint32_t>((occ.hi >> ((r - 7) * 9)) & 127ULL);
}

// Place a 9-bit rank mask `mask9` back into row `r` of a Bitboard.
inline Bitboard rank_mask_to_bb(int r, uint16_t mask9) {
    if (r <= 6) {
        return Bitboard(static_cast<uint64_t>(mask9) << (r * 9), 0ULL);
    }
    if (r == 7) {
        return Bitboard(static_cast<uint64_t>(mask9 & 1U) << 63, static_cast<uint64_t>(mask9 >> 1));
    }
    return Bitboard(0ULL, static_cast<uint64_t>(mask9) << 8);
}

inline Bitboard rank_attacks(int sq, const Bitboard& occ) {
    const int r = sq_row(sq);
    const int c = sq_col(sq);
    return rank_mask_to_bb(r, RANK_ATTACK_MASK[c][rank_inner_occ7(r, occ)]);
}

inline Bitboard file_attacks(int sq, const Bitboard& occ) {
    return ray_neg_bswap_branchless(BB_RAY_ORTHO[sq][0], occ) |
           ray_pos_branchless(BB_RAY_ORTHO[sq][2], occ);
}

inline Bitboard ray_ortho_attack(int sq, int dir, const Bitboard& occ) {
    if (dir == 0) return ray_neg_bswap_branchless(BB_RAY_ORTHO[sq][0], occ);
    if (dir == 2) return ray_pos_branchless(BB_RAY_ORTHO[sq][2], occ);
    const int r = sq_row(sq);
    const int c = sq_col(sq);
    const uint32_t occ7 = rank_inner_occ7(r, occ);
    return rank_mask_to_bb(r, (dir == 1) ? RANK_RAY_POS_MASK[c][occ7] : RANK_RAY_NEG_MASK[c][occ7]);
}

inline Bitboard ortho_attacks(int sq, const Bitboard& occ) {
    return rank_attacks(sq, occ) | file_attacks(sq, occ);
}

inline Bitboard diag_attacks(int sq, const Bitboard& occ) {
    return ray_neg_bswap_branchless(BB_RAY_DIAG[sq][0], occ) |
           ray_neg_bswap_branchless(BB_RAY_DIAG[sq][1], occ) |
           ray_pos_branchless(BB_RAY_DIAG[sq][2], occ) |
           ray_pos_branchless(BB_RAY_DIAG[sq][3], occ);
}

inline int ray_ortho_blocker(int sq, int dir, const Bitboard& occ) {
    Bitboard blockers = BB_RAY_ORTHO[sq][dir] & occ;
    if (!blockers.any()) return -1;
    return (dir == 1 || dir == 2) ? blockers.lsb() : blockers.msb();
}

inline int ray_diag_blocker(int sq, int dir, const Bitboard& occ) {
    Bitboard blockers = BB_RAY_DIAG[sq][dir] & occ;
    if (!blockers.any()) return -1;
    return (dir >= 2) ? blockers.lsb() : blockers.msb();
}

inline Bitboard piece_attacks_bb_occ(int sq, uint8_t pt, uint8_t facing, const Bitboard& occ) {
    if (pt == LONG) return ortho_attacks(sq, occ);
    if (pt == PHUONG) return diag_attacks(sq, occ);
    if (pt == XA) return ray_ortho_attack(sq, facing & 3, occ);
    return BB_STEP_ATTACKS[pt][facing & 3][sq];
}

// Zero atomic loads! Tables initialized once at startup via init_homei_tables().
inline Bitboard piece_attacks_bb(const Position& pos, int sq, uint8_t pt, uint8_t facing) {
    return piece_attacks_bb_occ(sq, pt, facing, pos.bb_occ);
}

} // namespace homei

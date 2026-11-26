#pragma once

#include "nnue.h"
#include "../types.h"
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace homei::nnue {

// Compact architecture-independent binary sample (104 bytes exact)
// Can be re-decoded into ANY future NNUE architecture without re-generating selfplay!
struct PackedPiece {
    uint8_t sq;   // 0..80
    uint8_t meta; // bits [7..6]: level (0=top, 1=buried1, 2=buried>=2); bit 5: owner; bits [4..2]: ptype; bits [1..0]: facing
};

struct PackedSample {
    uint8_t side_to_move;       // 0=CAM, 1=XANH
    uint8_t reserve[2];         // RESERVE_LONG | RESERVE_PHUONG bitmask
    uint8_t num_pieces;         // <= 40
    uint8_t prisoners[2][8];    // 16 bytes
    PackedPiece pieces[40];     // 80 bytes
    int16_t score;              // centipawns from side_to_move perspective
    int8_t  wdl;                // +1 STM win, 0 draw, -1 STM loss
    uint8_t ply;                // clamped 0..255
};
static_assert(sizeof(PackedSample) == 104, "PackedSample must be exactly 104 bytes");

inline PackedSample pack_position(const Position& pos, int16_t score, int8_t wdl, int ply) {
    PackedSample out{};
    out.side_to_move = pos.side_to_move & 1U;
    out.reserve[0] = pos.reserve[0];
    out.reserve[1] = pos.reserve[1];
    std::memcpy(out.prisoners, pos.prisoners, sizeof(out.prisoners));
    out.score = score;
    out.wdl = wdl;
    out.ply = static_cast<uint8_t>(std::clamp(ply, 0, 255));

    uint8_t n = 0;
    Bitboard occ = pos.bb_occ;
    while (occ.any() && n < 40) {
        int sq = occ.pop_lsb();
        const Piece& top = pos.top_piece[sq];
        out.pieces[n++] = PackedPiece{
            static_cast<uint8_t>(sq),
            static_cast<uint8_t>((0U << 6) | ((top.owner & 1U) << 5) | ((top.type & 7U) << 2) | (top.facing & 3U))
        };
        if (pos.height[sq] >= 2) {
            uint8_t idx = pos.buried_head[sq];
            uint8_t lvl = 1;
            while (idx != BURIED_NONE && n < 40) {
                const Piece& bp = pos.buried_piece[idx];
                uint8_t enc_lvl = (lvl == 1) ? 1U : 2U;
                out.pieces[n++] = PackedPiece{
                    static_cast<uint8_t>(sq),
                    static_cast<uint8_t>((enc_lvl << 6) | ((bp.owner & 1U) << 5) | ((bp.type & 7U) << 2) | (bp.facing & 3U))
                };
                idx = pos.buried_next[idx];
                if (lvl < 2) ++lvl;
            }
        }
    }
    out.num_pieces = n;
    return out;
}

// Directly extract CLP-HalfKA_hm global feature indices (0..INPUT_SIZE-1) from a PackedSample
// with optional horizontal mirror augmentation (`aug_mirror_h`) — zero Position reconstruction needed!
inline int extract_global_features_packed(
    const PackedSample& s,
    int persp,
    bool aug_mirror_h,
    int32_t out_global[MAX_ACTIVE_FEATS],
    int32_t out_local_shared[MAX_ACTIVE_FEATS]
) {
    // Find own king square first
    int king_sq = 255;
    for (int i = 0; i < s.num_pieces; ++i) {
        uint8_t meta = s.pieces[i].meta;
        uint8_t owner = (meta >> 5) & 1U;
        uint8_t ptype = (meta >> 2) & 7U;
        if (owner == static_cast<uint8_t>(persp) && ptype == VUONG) {
            king_sq = s.pieces[i].sq;
            break;
        }
    }

    const bool rot180 = (persp == XANH);
    int kr = 0, kc = 4;
    if (king_sq < NUM_SQUARES) {
        kr = king_sq / 9;
        kc = king_sq % 9;
        if (aug_mirror_h) kc = 8 - kc;
        if (rot180) {
            kr = 8 - kr;
            kc = 8 - kc;
        }
    }
    const bool flip_h = (kc > 4);
    const int kc_hm = flip_h ? (8 - kc) : kc;
    const int kr_zone = (kr < 3) ? kr : 3;
    const int bucket = kr_zone * 5 + kc_hm;
    const int bucket_off = bucket * F_LOCAL;

    // Net horizontal flip after optional augmentation + king-side mirror
    const bool net_flip_board_col = (aug_mirror_h != flip_h);

    int count = 0;
    uint8_t deep_cnt[2][8]{};

    for (int i = 0; i < s.num_pieces; ++i) {
        int sq = s.pieces[i].sq;
        uint8_t meta = s.pieces[i].meta;
        uint8_t lvl = (meta >> 6) & 3U;
        uint8_t owner = (meta >> 5) & 1U;
        uint8_t ptype = (meta >> 2) & 7U;
        uint8_t facing = meta & 3U;
        int rel_owner = (owner == static_cast<uint8_t>(persp)) ? 0 : 1;

        if (lvl >= 2) {
            if (deep_cnt[rel_owner][ptype] < 2) ++deep_cnt[rel_owner][ptype];
            continue;
        }

        int r = sq / 9;
        int c = sq % 9;
        if (aug_mirror_h) {
            c = 8 - c;
            if (can_rotate(ptype)) facing = mirror_facing_h(facing);
        }
        if (rot180) {
            r = 8 - r;
            c = 8 - c;
            if (can_rotate(ptype)) facing = static_cast<uint8_t>((facing + 2) & 3);
        }
        if (flip_h) {
            c = 8 - c;
            if (can_rotate(ptype)) facing = mirror_facing_h(facing);
        }
        int tsq = r * 9 + c;

        int local = (lvl == 0)
            ? (F_TOP_OFF + tsq * 46 + rel_owner * 23 + pf23_idx(ptype, facing))
            : (F_B1_OFF + tsq * 16 + rel_owner * 8 + ptype);

        if (count < MAX_ACTIVE_FEATS) {
            out_local_shared[count] = local;
            out_global[count] = bucket_off + local;
            ++count;
        }
    }
    (void)net_flip_board_col;

    for (int ro = 0; ro < 2; ++ro) {
        for (int pt = 0; pt < 8; ++pt) {
            uint8_t dc = deep_cnt[ro][pt];
            for (uint8_t k = 0; k < dc && count < MAX_ACTIVE_FEATS; ++k) {
                int local = F_BDEEP_OFF + ro * 16 + pt * 2 + k;
                out_local_shared[count] = local;
                out_global[count] = bucket_off + local;
                ++count;
            }
        }
    }

    for (int ro = 0; ro < 2; ++ro) {
        int abs_side = (ro == 0) ? persp : (1 - persp);
        uint8_t res = s.reserve[abs_side];
        if ((res & RESERVE_LONG) && count < MAX_ACTIVE_FEATS) {
            int local = F_RES_OFF + ro * 2 + 0;
            out_local_shared[count] = local;
            out_global[count] = bucket_off + local;
            ++count;
        }
        if ((res & RESERVE_PHUONG) && count < MAX_ACTIVE_FEATS) {
            int local = F_RES_OFF + ro * 2 + 1;
            out_local_shared[count] = local;
            out_global[count] = bucket_off + local;
            ++count;
        }
    }

    for (int ro = 0; ro < 2; ++ro) {
        int abs_side = (ro == 0) ? persp : (1 - persp);
        for (int pt = 1; pt < 8; ++pt) {
            int cnt = std::min<int>(s.prisoners[abs_side][pt], PRIS_MAX[pt]);
            int base = F_PRIS_OFF + ro * 38 + PRIS_OFF[pt];
            for (int k = 0; k < cnt && count < MAX_ACTIVE_FEATS; ++k) {
                int local = base + k;
                out_local_shared[count] = local;
                out_global[count] = bucket_off + local;
                ++count;
            }
        }
    }

    return count;
}

inline int select_stack_packed(const PackedSample& s) {
    int tot = 0;
    for (int sd = 0; sd < 2; ++sd)
        for (int pt = 1; pt < 8; ++pt)
            tot += s.prisoners[sd][pt];
    int idx = tot >> 1;
    return (idx < NUM_STACKS) ? idx : (NUM_STACKS - 1);
}

} // namespace homei::nnue

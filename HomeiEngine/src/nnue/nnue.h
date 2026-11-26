#pragma once

#include "../types.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace homei::nnue {

// ============================================================================
// CLP-HalfKA_hm SFNN v1 Architecture Constants
//   - 20 Smart Horizontal-Mirrored King Buckets: min(kr_rot, 3) * 5 + kc_hm
//   - 5,134 local features per bucket:
//       [   0..3725] Top pieces (81 squares x 2 owners x 23 piece-facing states)
//       [3726..5021] Buried-1 pieces immediately below top (81 squares x 2 owners x 8 piece types)
//       [5022..5053] Buried-deep pieces >= 2 below top (2 owners x 8 types x 2 thermometer)
//       [5054..5057] Reserve hand (2 owners x 2: LONG, PHUONG)
//       [5058..5133] Prisoners hand (2 owners x 38 thermometer units)
//   - Total sparse input: 20 * 5,134 = 102,680
//   - Feature Transformer: int16[FT_HALF=256] per perspective (fits in 16 AVX2 YMM registers!)
//   - Activation: SCReLU -> uint8[512] = [stm(256), opp(256)]
//   - 8 Layer Stacks (selected by clamp(total_prisoners / 2, 0, 7)):
//       FC0: 512 -> 16 (15 hidden CReLU + 1 direct linear skip)
//       FC1: 16  -> 32 (CReLU)
//       FC2: 32  -> 1  (+ skip -> centipawns)
// ============================================================================

constexpr uint32_t NNUE_MAGIC   = 0x314E4D48U; // "HMN1"
constexpr uint32_t NNUE_VERSION = 1U;

constexpr int KING_BUCKETS   = 20;
constexpr int F_TOP_OFF      = 0;
constexpr int F_TOP_COUNT    = 81 * 46;          // 3726
constexpr int F_B1_OFF       = F_TOP_OFF + F_TOP_COUNT; // 3726
constexpr int F_B1_COUNT     = 81 * 16;          // 1296
constexpr int F_BDEEP_OFF    = F_B1_OFF + F_B1_COUNT;   // 5022
constexpr int F_BDEEP_COUNT  = 32;               // 2 * 8 * 2
constexpr int F_RES_OFF      = F_BDEEP_OFF + F_BDEEP_COUNT; // 5054
constexpr int F_RES_COUNT    = 4;                // 2 * 2
constexpr int F_PRIS_OFF     = F_RES_OFF + F_RES_COUNT; // 5058
constexpr int F_PRIS_COUNT   = 76;               // 2 * 38
constexpr int F_LOCAL        = F_PRIS_OFF + F_PRIS_COUNT; // 5134
constexpr int INPUT_SIZE     = KING_BUCKETS * F_LOCAL;    // 102,680

constexpr int FT_HALF        = 256;
constexpr int FT_TOTAL       = FT_HALF * 2;      // 512
constexpr int NUM_STACKS     = 8;
constexpr int L1_OUT         = 16;               // 15 hidden + slot 15 = linear skip
constexpr int L2_OUT         = 32;

// Quantization scales (must match train_nnue.py bit-for-bit!)
constexpr int QA             = 128;              // FT & hidden activation scale (clamp 0..127)
constexpr int QW0            = 128;              // FC0 int8 weight scale ([-0.992, +0.992])
constexpr int QW             = 64;               // FC1/FC2 int8 weight scale ([-1.984, +1.984])
constexpr int EVAL_SCALE     = 400;              // Raw output / (QA * QW) * EVAL_SCALE -> cp

constexpr int MAX_ACTIVE_FEATS = 96;

inline constexpr uint8_t PF23_BASE[8] = {0, 1, 5, 9, 13, 17, 18, 19};
inline constexpr uint8_t PRIS_MAX[8]  = {0, 4, 4, 4, 4, 2, 2, 18};
inline constexpr uint8_t PRIS_OFF[8]  = {0, 0, 4, 8, 12, 16, 18, 20};

inline constexpr int pf23_idx(uint8_t ptype, uint8_t facing) {
    uint8_t pt = ptype & 7U;
    return static_cast<int>(PF23_BASE[pt]) + (can_rotate(pt) ? static_cast<int>(facing & 3U) : 0);
}

inline constexpr uint8_t mirror_facing_h(uint8_t f) {
    return (f == 1) ? 3U : ((f == 3) ? 1U : (f & 3U));
}

// Compute perspective king bucket (0..19) and horizontal flip flag for `persp`.
inline void king_bucket_and_flip(const Position& pos, int persp, int& out_bucket, bool& out_flip) {
    int ksq = pos.king_sq[persp];
    if (ksq >= NUM_SQUARES) {
        out_bucket = 4; // center home fallback
        out_flip = false;
        return;
    }
    int kr = sq_row(ksq);
    int kc = sq_col(ksq);
    if (persp == XANH) {
        kr = 8 - kr;
        kc = 8 - kc;
    }
    out_flip = (kc > 4);
    int kc_hm = out_flip ? (8 - kc) : kc;
    int kr_zone = (kr < 3) ? kr : 3;
    out_bucket = kr_zone * 5 + kc_hm;
}

inline int select_layer_stack(const Position& pos) {
    int tot = 0;
    for (int s = 0; s < 2; ++s) {
        for (int pt = 1; pt < 8; ++pt) {
            tot += pos.prisoners[s][pt];
        }
    }
    int idx = tot >> 1;
    return (idx < NUM_STACKS) ? idx : (NUM_STACKS - 1);
}

// Layer stack weights (aligned to 32 bytes for AVX2)
struct alignas(32) LayerStack {
    alignas(32) int8_t  w0[L1_OUT][FT_TOTAL]; // 16 x 512 = 8192 B
    alignas(32) int32_t b0[L1_OUT];           // 64 B
    alignas(32) int8_t  w1[L2_OUT][L1_OUT];   // 32 x 16 = 512 B
    alignas(32) int32_t b1[L2_OUT];           // 128 B
    alignas(32) int8_t  w2[L2_OUT];           // 32 B
    int32_t b2;
};

struct Network {
    // Flat aligned FT weight matrix: INPUT_SIZE * FT_HALF int16_t (~52.6 MB)
    std::vector<int16_t> ft_weights; // allocated aligned or padded to 32B boundary
    int16_t* ft_w_aligned = nullptr;
    alignas(32) int16_t ft_bias[FT_HALF]{};
    alignas(32) LayerStack stacks[NUM_STACKS]{};
    bool loaded = false;
    uint64_t id = 0;
    std::string path;

    bool load_file(const std::string& file_path, std::string& err);
    void init_zero_for_test();
    void init_random_for_test(uint64_t seed = 0x123456789ABCDEF0ULL);
};

// Accumulator state for a single ply in the search stack
struct alignas(32) Accumulator {
    alignas(32) int16_t v[2][FT_HALF];
    uint8_t bucket[2]{255, 255};
    uint8_t flip[2]{0, 0};
    bool computed[2]{false, false};
};

// Finny Table entry per (perspective, king_bucket, flip_bit) for fast differential king refreshes
struct alignas(32) FinnyEntry {
    alignas(32) int16_t acc[FT_HALF];
    uint16_t feats[MAX_ACTIVE_FEATS]; // sorted local features (0..5133)
    uint8_t count = 0;
    bool valid = false;
};

struct FinnyTable {
    FinnyEntry entries[2][KING_BUCKETS][2];
    void clear() {
        for (int p = 0; p < 2; ++p)
            for (int b = 0; b < KING_BUCKETS; ++b)
                for (int f = 0; f < 2; ++f)
                    entries[p][b][f].valid = false;
    }
};

// Per-ply compact feature list snapshot for O(delta) incremental updates across any CLP move
struct alignas(32) PlyAccNode {
    Accumulator acc;
    // Cached sorted local feature lists per perspective (at most ~48 active features -> 96 bytes!)
    uint16_t local_feats[2][MAX_ACTIVE_FEATS];
    uint8_t feat_count[2]{0, 0};
    bool feats_valid[2]{false, false};
};

// Extract sorted local feature indices (0..F_LOCAL-1) for perspective `persp`.
int extract_local_features(const Position& pos, int persp, int& out_bucket, bool& out_flip, uint16_t out_local[MAX_ACTIVE_FEATS]);

// Full refresh into `acc.v[persp]` (uses FinnyTable if provided).
void refresh_perspective(const Network& net, const Position& pos, int persp, PlyAccNode& node, FinnyTable* finny = nullptr);

// Incremental update of `child` from `parent` (diffs sorted local features — handles ALL CLP moves:
// Single, Team, Split, Deep Stack Capture, Rotate, Reserve/Prisoner Drop with 100% mathematical exactness!).
void update_perspective(const Network& net, const Position& pos, int persp, const PlyAccNode& parent, PlyAccNode& child, FinnyTable* finny = nullptr);

// Evaluate from already-computed accumulator `node.acc` for `pos.side_to_move`.
int evaluate_accumulator(const Network& net, const Position& pos, const Accumulator& acc);

// Convenience full-refresh evaluation (used for root/testing/stand-alone eval).
int evaluate_position(const Network& net, const Position& pos);

// Global active network accessors (thread-safe shared_ptr snapshot)
bool load_global_network(const std::string& path, std::string& err);
void unload_global_network();
std::shared_ptr<const Network> get_global_network();
void set_nnue_enabled(bool enabled);
bool is_nnue_enabled();
std::string get_global_network_path();

} // namespace homei::nnue

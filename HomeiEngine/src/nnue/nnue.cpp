#include "nnue.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
#include <immintrin.h>
#define HOMEI_USE_AVX2 1
#else
#define HOMEI_USE_AVX2 0
#endif

namespace homei::nnue {

namespace {

std::mutex g_net_mtx;
std::shared_ptr<const Network> g_net;
std::atomic<bool> g_nnue_enabled{true};
std::atomic<uint64_t> g_next_net_id{1};

inline int16_t* align_ptr_32(std::vector<int16_t>& buf, size_t count) {
    buf.assign(count + 16, 0);
    uintptr_t p = reinterpret_cast<uintptr_t>(buf.data());
    uintptr_t aligned = (p + 31U) & ~static_cast<uintptr_t>(31U);
    return reinterpret_cast<int16_t*>(aligned);
}

inline void vec_copy_256(int16_t* dst, const int16_t* src) {
    std::memcpy(dst, src, FT_HALF * sizeof(int16_t));
}

inline void vec_add_256(int16_t* dst, const int16_t* w) {
#if HOMEI_USE_AVX2
    for (int i = 0; i < FT_HALF; i += 16) {
        __m256i a = _mm256_load_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i b = _mm256_load_si256(reinterpret_cast<const __m256i*>(w + i));
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_add_epi16(a, b));
    }
#else
    for (int i = 0; i < FT_HALF; ++i) dst[i] = static_cast<int16_t>(dst[i] + w[i]);
#endif
}

inline void vec_sub_256(int16_t* dst, const int16_t* w) {
#if HOMEI_USE_AVX2
    for (int i = 0; i < FT_HALF; i += 16) {
        __m256i a = _mm256_load_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i b = _mm256_load_si256(reinterpret_cast<const __m256i*>(w + i));
        _mm256_store_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_sub_epi16(a, b));
    }
#else
    for (int i = 0; i < FT_HALF; ++i) dst[i] = static_cast<int16_t>(dst[i] - w[i]);
#endif
}

#if HOMEI_USE_AVX2
inline int32_t hsum_epi32_256(__m256i s) {
    __m128i lo = _mm256_castsi256_si128(s);
    __m128i hi = _mm256_extracti128_si256(s, 1);
    __m128i v = _mm_add_epi32(lo, hi);
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
    return _mm_cvtsi128_si32(v);
}

// Compute 4 dot products of uint8_t[512] against 4 int8_t[512] rows simultaneously (4x less ft_out loads!)
inline void dot_u8_i8_512_x4(
    const uint8_t* a,
    const int8_t* b0,
    const int8_t* b1,
    const int8_t* b2,
    const int8_t* b3,
    int32_t out[4]
) {
    const __m256i ones = _mm256_set1_epi16(1);
    __m256i s0 = _mm256_setzero_si256();
    __m256i s1 = _mm256_setzero_si256();
    __m256i s2 = _mm256_setzero_si256();
    __m256i s3 = _mm256_setzero_si256();
    for (int i = 0; i < FT_TOTAL; i += 32) {
        __m256i va = _mm256_load_si256(reinterpret_cast<const __m256i*>(a + i));
        s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(_mm256_maddubs_epi16(va, _mm256_load_si256(reinterpret_cast<const __m256i*>(b0 + i))), ones));
        s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(_mm256_maddubs_epi16(va, _mm256_load_si256(reinterpret_cast<const __m256i*>(b1 + i))), ones));
        s2 = _mm256_add_epi32(s2, _mm256_madd_epi16(_mm256_maddubs_epi16(va, _mm256_load_si256(reinterpret_cast<const __m256i*>(b2 + i))), ones));
        s3 = _mm256_add_epi32(s3, _mm256_madd_epi16(_mm256_maddubs_epi16(va, _mm256_load_si256(reinterpret_cast<const __m256i*>(b3 + i))), ones));
    }
    out[0] = hsum_epi32_256(s0);
    out[1] = hsum_epi32_256(s1);
    out[2] = hsum_epi32_256(s2);
    out[3] = hsum_epi32_256(s3);
}

inline int32_t dot_u8_i8_16(const uint8_t* a, const int8_t* b) {
    __m128i va = _mm_load_si128(reinterpret_cast<const __m128i*>(a));
    __m128i vb = _mm_load_si128(reinterpret_cast<const __m128i*>(b));
    __m128i p  = _mm_maddubs_epi16(va, vb);
    __m128i v  = _mm_madd_epi16(p, _mm_set1_epi16(1));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
    v = _mm_add_epi32(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
    return _mm_cvtsi128_si32(v);
}

inline int32_t dot_u8_i8_32(const uint8_t* a, const int8_t* b) {
    __m256i va = _mm256_load_si256(reinterpret_cast<const __m256i*>(a));
    __m256i vb = _mm256_load_si256(reinterpret_cast<const __m256i*>(b));
    __m256i p  = _mm256_maddubs_epi16(va, vb);
    return hsum_epi32_256(_mm256_madd_epi16(p, _mm256_set1_epi16(1)));
}
#endif

inline void sort_u16_small(uint16_t* arr, int n) {
    for (int i = 1; i < n; ++i) {
        uint16_t key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            --j;
        }
        arr[j + 1] = key;
    }
}

// Compute SCReLU: out[i] = (clamp(in[i], 0, 127)^2) >> 7  (in [0..126])
inline void screlu_256(const int16_t* in, uint8_t* out) {
#if HOMEI_USE_AVX2
    const __m256i vzero = _mm256_setzero_si256();
    const __m256i vmax  = _mm256_set1_epi16(127);
    for (int i = 0; i < FT_HALF; i += 32) {
        __m256i x0 = _mm256_load_si256(reinterpret_cast<const __m256i*>(in + i));
        __m256i x1 = _mm256_load_si256(reinterpret_cast<const __m256i*>(in + i + 16));
        x0 = _mm256_min_epi16(_mm256_max_epi16(x0, vzero), vmax);
        x1 = _mm256_min_epi16(_mm256_max_epi16(x1, vzero), vmax);
        x0 = _mm256_srli_epi16(_mm256_mullo_epi16(x0, x0), 7);
        x1 = _mm256_srli_epi16(_mm256_mullo_epi16(x1, x1), 7);
        __m256i packed = _mm256_packus_epi16(x0, x1);
        packed = _mm256_permute4x64_epi64(packed, 0xD8); // fix cross-lane packus order!
        _mm256_store_si256(reinterpret_cast<__m256i*>(out + i), packed);
    }
#else
    for (int i = 0; i < FT_HALF; ++i) {
        int c = std::clamp<int>(in[i], 0, 127);
        out[i] = static_cast<uint8_t>((c * c) >> 7);
    }
#endif
}

// Apply sorted feature diff between old_f[0..old_n) and new_f[0..new_n) into acc[0..256)
inline void apply_sorted_diff(
    const int16_t* bucket_w_base,
    int16_t* acc,
    const uint16_t* old_f,
    int old_n,
    const uint16_t* new_f,
    int new_n
) {
    int i = 0, j = 0;
    while (i < old_n && j < new_n) {
        uint16_t a = old_f[i];
        uint16_t b = new_f[j];
        if (a == b) {
            ++i;
            ++j;
        } else if (a < b) {
            vec_sub_256(acc, bucket_w_base + static_cast<size_t>(a) * FT_HALF);
            ++i;
        } else {
            vec_add_256(acc, bucket_w_base + static_cast<size_t>(b) * FT_HALF);
            ++j;
        }
    }
    while (i < old_n) {
        vec_sub_256(acc, bucket_w_base + static_cast<size_t>(old_f[i++]) * FT_HALF);
    }
    while (j < new_n) {
        vec_add_256(acc, bucket_w_base + static_cast<size_t>(new_f[j++]) * FT_HALF);
    }
}

} // namespace

int extract_local_features(const Position& pos, int persp, int& out_bucket, bool& out_flip, uint16_t out_local[MAX_ACTIVE_FEATS]) {
    king_bucket_and_flip(pos, persp, out_bucket, out_flip);
    const bool rot180 = (persp == XANH);
    const bool flip_h = out_flip;

    int count = 0;
    uint16_t b1_buf[40];
    int b1_count = 0;
    uint8_t deep_cnt[2][8]{};

    // Iterate occupied squares via bitboard (only ~20..36 squares!)
    Bitboard occ = pos.bb_occ;
    while (occ.any()) {
        int sq = occ.pop_lsb();
        int r = sq_row(sq);
        int c = sq_col(sq);
        if (rot180) {
            r = 8 - r;
            c = 8 - c;
        }
        if (flip_h) {
            c = 8 - c;
        }
        int tsq = r * 9 + c;

        // 1. Top piece
        const Piece& top = pos.top_piece[sq];
        int rel_owner = (top.owner == persp) ? 0 : 1;
        uint8_t rf = top.facing;
        if (rot180 && can_rotate(top.type)) rf = static_cast<uint8_t>((rf + 2) & 3);
        if (flip_h && can_rotate(top.type)) rf = mirror_facing_h(rf);
        if (count < MAX_ACTIVE_FEATS) {
            out_local[count++] = static_cast<uint16_t>(F_TOP_OFF + tsq * 46 + rel_owner * 23 + pf23_idx(top.type, rf));
        }

        // 2. Buried pieces (buried_head is immediately underneath top -> Buried-1!)
        if (pos.height[sq] >= 2) {
            uint8_t idx = pos.buried_head[sq];
            if (idx != BURIED_NONE) {
                const Piece& b1 = pos.buried_piece[idx];
                int b1_owner = (b1.owner == persp) ? 0 : 1;
                if (b1_count < 40) {
                    b1_buf[b1_count++] = static_cast<uint16_t>(F_B1_OFF + tsq * 16 + b1_owner * 8 + (b1.type & 7U));
                }
                idx = pos.buried_next[idx];
                while (idx != BURIED_NONE) {
                    const Piece& bd = pos.buried_piece[idx];
                    int bd_owner = (bd.owner == persp) ? 0 : 1;
                    if (deep_cnt[bd_owner][bd.type & 7U] < 2) {
                        ++deep_cnt[bd_owner][bd.type & 7U];
                    }
                    idx = pos.buried_next[idx];
                }
            }
        }
    }

    // Top & Buried-1 blocks are visited in strictly ascending sq order!
    // (!rot180 && !flip_h) -> already sorted; (rot180 && !flip_h) -> strictly reversed -> O(N) std::reverse!
    // (flip_h) -> at most within-row inversions after optional reverse -> O(N) insertion sort!
    if (rot180) {
        std::reverse(out_local, out_local + count);
        if (flip_h) sort_u16_small(out_local, count);
    } else if (flip_h) {
        sort_u16_small(out_local, count);
    }
    if (b1_count > 0) {
        if (rot180) {
            std::reverse(b1_buf, b1_buf + b1_count);
            if (flip_h) sort_u16_small(b1_buf, b1_count);
        } else if (flip_h) {
            sort_u16_small(b1_buf, b1_count);
        }
        int copy_n = std::min(b1_count, MAX_ACTIVE_FEATS - count);
        std::memcpy(out_local + count, b1_buf, static_cast<size_t>(copy_n) * sizeof(uint16_t));
        count += copy_n;
    }

    // 3. Deep buried counts (strictly ascending!)
    for (int ro = 0; ro < 2; ++ro) {
        for (int pt = 0; pt < 8; ++pt) {
            uint8_t dc = deep_cnt[ro][pt];
            for (uint8_t k = 0; k < dc && count < MAX_ACTIVE_FEATS; ++k) {
                out_local[count++] = static_cast<uint16_t>(F_BDEEP_OFF + ro * 16 + pt * 2 + k);
            }
        }
    }

    // 4. Reserve hands (strictly ascending!)
    for (int ro = 0; ro < 2; ++ro) {
        int abs_side = (ro == 0) ? persp : (1 - persp);
        uint8_t res = pos.reserve[abs_side];
        if ((res & RESERVE_LONG) && count < MAX_ACTIVE_FEATS) {
            out_local[count++] = static_cast<uint16_t>(F_RES_OFF + ro * 2 + 0);
        }
        if ((res & RESERVE_PHUONG) && count < MAX_ACTIVE_FEATS) {
            out_local[count++] = static_cast<uint16_t>(F_RES_OFF + ro * 2 + 1);
        }
    }

    // 5. Prisoner hands (strictly ascending!)
    for (int ro = 0; ro < 2; ++ro) {
        int abs_side = (ro == 0) ? persp : (1 - persp);
        for (int pt = 1; pt < 8; ++pt) {
            int cnt = std::min<int>(pos.prisoners[abs_side][pt], PRIS_MAX[pt]);
            int base = F_PRIS_OFF + ro * 38 + PRIS_OFF[pt];
            for (int k = 0; k < cnt && count < MAX_ACTIVE_FEATS; ++k) {
                out_local[count++] = static_cast<uint16_t>(base + k);
            }
        }
    }

    return count;
}

void refresh_perspective(const Network& net, const Position& pos, int persp, PlyAccNode& node, FinnyTable* finny) {
    int bucket = 0;
    bool flip = false;
    int cnt = extract_local_features(pos, persp, bucket, flip, node.local_feats[persp]);
    node.feat_count[persp] = static_cast<uint8_t>(cnt);
    node.feats_valid[persp] = true;
    node.acc.bucket[persp] = static_cast<uint8_t>(bucket);
    node.acc.flip[persp] = flip ? 1U : 0U;

    const int16_t* w_base = net.ft_w_aligned + static_cast<size_t>(bucket) * F_LOCAL * FT_HALF;
    int16_t* dst = node.acc.v[persp];

    if (finny) {
        FinnyEntry& fe = finny->entries[persp][bucket][flip ? 1 : 0];
        if (fe.valid) {
            vec_copy_256(dst, fe.acc);
            apply_sorted_diff(w_base, dst, fe.feats, fe.count, node.local_feats[persp], cnt);
            vec_copy_256(fe.acc, dst);
            std::memcpy(fe.feats, node.local_feats[persp], static_cast<size_t>(cnt) * sizeof(uint16_t));
            fe.count = static_cast<uint8_t>(cnt);
            node.acc.computed[persp] = true;
            return;
        }
    }

    vec_copy_256(dst, net.ft_bias);
    for (int i = 0; i < cnt; ++i) {
        vec_add_256(dst, w_base + static_cast<size_t>(node.local_feats[persp][i]) * FT_HALF);
    }

    if (finny) {
        FinnyEntry& fe = finny->entries[persp][bucket][flip ? 1 : 0];
        vec_copy_256(fe.acc, dst);
        std::memcpy(fe.feats, node.local_feats[persp], static_cast<size_t>(cnt) * sizeof(uint16_t));
        fe.count = static_cast<uint8_t>(cnt);
        fe.valid = true;
    }
    node.acc.computed[persp] = true;
}

void update_perspective(const Network& net, const Position& pos, int persp, const PlyAccNode& parent, PlyAccNode& child, FinnyTable* finny) {
    int bucket = 0;
    bool flip = false;
    king_bucket_and_flip(pos, persp, bucket, flip);

    if (!parent.acc.computed[persp] || !parent.feats_valid[persp] ||
        parent.acc.bucket[persp] != static_cast<uint8_t>(bucket) ||
        parent.acc.flip[persp] != (flip ? 1U : 0U)) {
        refresh_perspective(net, pos, persp, child, finny);
        return;
    }

    int cnt = extract_local_features(pos, persp, bucket, flip, child.local_feats[persp]);
    child.feat_count[persp] = static_cast<uint8_t>(cnt);
    child.feats_valid[persp] = true;
    child.acc.bucket[persp] = static_cast<uint8_t>(bucket);
    child.acc.flip[persp] = flip ? 1U : 0U;

    const int16_t* w_base = net.ft_w_aligned + static_cast<size_t>(bucket) * F_LOCAL * FT_HALF;
    int16_t* dst = child.acc.v[persp];
    vec_copy_256(dst, parent.acc.v[persp]);
    apply_sorted_diff(w_base, dst, parent.local_feats[persp], parent.feat_count[persp], child.local_feats[persp], cnt);
    child.acc.computed[persp] = true;
}

int evaluate_accumulator(const Network& net, const Position& pos, const Accumulator& acc) {
    const int stm = pos.side_to_move & 1;
    const int opp = stm ^ 1;

    alignas(32) uint8_t ft_out[FT_TOTAL];
    screlu_256(acc.v[stm], ft_out);
    screlu_256(acc.v[opp], ft_out + FT_HALF);

    const LayerStack& st = net.stacks[select_layer_stack(pos)];

    // FC0: 512 -> 16 (QW0 = 128 -> slots 0..14 CReLU >> 7, slot 15 linear skip >> 1)
    alignas(32) uint8_t h0[L1_OUT];
    int32_t skip = 0;
#if HOMEI_USE_AVX2
    for (int j = 0; j < L1_OUT; j += 4) {
        int32_t d[4];
        dot_u8_i8_512_x4(ft_out, st.w0[j], st.w0[j + 1], st.w0[j + 2], st.w0[j + 3], d);
        for (int t = 0; t < 4; ++t) {
            int idx = j + t;
            int32_t sum = d[t] + st.b0[idx];
            if (idx == L1_OUT - 1) {
                skip = sum >> 1; // scale QA*QW0 (16384) -> QA*QW (8192)
                h0[idx] = 0;
            } else {
                int32_t act = sum >> 7; // divide by QW0 (128) -> scale QA (128)
                h0[idx] = static_cast<uint8_t>(std::clamp(act, 0, 127));
            }
        }
    }
#else
    for (int j = 0; j < L1_OUT; ++j) {
        int32_t sum = st.b0[j];
        for (int i = 0; i < FT_TOTAL; ++i) sum += static_cast<int32_t>(ft_out[i]) * static_cast<int32_t>(st.w0[j][i]);
        if (j == L1_OUT - 1) {
            skip = sum >> 1;
            h0[j] = 0;
        } else {
            h0[j] = static_cast<uint8_t>(std::clamp(sum >> 7, 0, 127));
        }
    }
#endif

    // FC1: 16 -> 32 (QW = 64 -> CReLU >> 6)
    alignas(32) uint8_t h1[L2_OUT];
    for (int k = 0; k < L2_OUT; ++k) {
#if HOMEI_USE_AVX2
        int32_t sum = dot_u8_i8_16(h0, st.w1[k]) + st.b1[k];
#else
        int32_t sum = st.b1[k];
        for (int j = 0; j < L1_OUT - 1; ++j) {
            sum += static_cast<int32_t>(h0[j]) * static_cast<int32_t>(st.w1[k][j]);
        }
#endif
        int32_t act = sum >> 6;
        h1[k] = static_cast<uint8_t>(std::clamp(act, 0, 127));
    }

    // FC2: 32 -> 1 (+ skip)
#if HOMEI_USE_AVX2
    int32_t out = dot_u8_i8_32(h1, st.w2) + st.b2;
#else
    int32_t out = st.b2;
    for (int k = 0; k < L2_OUT; ++k) {
        out += static_cast<int32_t>(h1[k]) * static_cast<int32_t>(st.w2[k]);
    }
#endif
    int64_t total = static_cast<int64_t>(out) + static_cast<int64_t>(skip);
    // Scale: total is in QA * QW = 128 * 64 = 8192 units -> convert to centipawns
    int cp = static_cast<int>((total * EVAL_SCALE) / (QA * QW));
    return std::clamp(cp, -15000, 15000);
}

int evaluate_position(const Network& net, const Position& pos) {
    PlyAccNode node;
    refresh_perspective(net, pos, CAM, node, nullptr);
    refresh_perspective(net, pos, XANH, node, nullptr);
    return evaluate_accumulator(net, pos, node.acc);
}

bool Network::load_file(const std::string& file_path, std::string& err) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs.is_open()) {
        err = "Cannot open NNUE file: " + file_path;
        return false;
    }

    struct Header {
        uint32_t magic;
        uint32_t version;
        uint32_t king_buckets;
        uint32_t f_local;
        uint32_t ft_half;
        uint32_t num_stacks;
        uint32_t l1_out;
        uint32_t l2_out;
    } hdr{};

    ifs.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!ifs || hdr.magic != NNUE_MAGIC || hdr.version != NNUE_VERSION) {
        err = "Invalid NNUE header/magic";
        return false;
    }
    if (hdr.king_buckets != KING_BUCKETS || hdr.f_local != F_LOCAL ||
        hdr.ft_half != FT_HALF || hdr.num_stacks != NUM_STACKS ||
        hdr.l1_out != L1_OUT || hdr.l2_out != L2_OUT) {
        err = "NNUE architecture mismatch";
        return false;
    }

    const size_t ft_elems = static_cast<size_t>(INPUT_SIZE) * FT_HALF;
    ft_w_aligned = align_ptr_32(ft_weights, ft_elems);
    ifs.read(reinterpret_cast<char*>(ft_w_aligned), static_cast<std::streamsize>(ft_elems * sizeof(int16_t)));
    ifs.read(reinterpret_cast<char*>(ft_bias), sizeof(ft_bias));
    for (int s = 0; s < NUM_STACKS; ++s) {
        ifs.read(reinterpret_cast<char*>(stacks[s].w0), sizeof(stacks[s].w0));
        ifs.read(reinterpret_cast<char*>(stacks[s].b0), sizeof(stacks[s].b0));
        ifs.read(reinterpret_cast<char*>(stacks[s].w1), sizeof(stacks[s].w1));
        ifs.read(reinterpret_cast<char*>(stacks[s].b1), sizeof(stacks[s].b1));
        ifs.read(reinterpret_cast<char*>(stacks[s].w2), sizeof(stacks[s].w2));
        ifs.read(reinterpret_cast<char*>(&stacks[s].b2), sizeof(stacks[s].b2));
    }
    if (!ifs) {
        err = "Truncated NNUE file: " + file_path;
        return false;
    }

    loaded = true;
    id = g_next_net_id.fetch_add(1, std::memory_order_relaxed);
    path = file_path;
    return true;
}

void Network::init_zero_for_test() {
    const size_t ft_elems = static_cast<size_t>(INPUT_SIZE) * FT_HALF;
    ft_w_aligned = align_ptr_32(ft_weights, ft_elems);
    std::memset(ft_bias, 0, sizeof(ft_bias));
    std::memset(stacks, 0, sizeof(stacks));
    loaded = true;
    id = g_next_net_id.fetch_add(1, std::memory_order_relaxed);
    path = "<zero-test>";
}

void Network::init_random_for_test(uint64_t seed) {
    auto next_u64 = [&seed]() -> uint64_t {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        return seed * 0x2545F4914F6CDD1DULL;
    };
    const size_t ft_elems = static_cast<size_t>(INPUT_SIZE) * FT_HALF;
    ft_w_aligned = align_ptr_32(ft_weights, ft_elems);
    for (size_t i = 0; i < ft_elems; ++i) {
        ft_w_aligned[i] = static_cast<int16_t>(static_cast<int>(next_u64() % 65) - 32);
    }
    for (int i = 0; i < FT_HALF; ++i) {
        ft_bias[i] = static_cast<int16_t>(static_cast<int>(next_u64() % 65) - 32);
    }
    for (int s = 0; s < NUM_STACKS; ++s) {
        for (int j = 0; j < L1_OUT; ++j) {
            for (int i = 0; i < FT_TOTAL; ++i) {
                stacks[s].w0[j][i] = static_cast<int8_t>(static_cast<int>(next_u64() % 33) - 16);
            }
            stacks[s].b0[j] = static_cast<int32_t>(static_cast<int>(next_u64() % 2049) - 1024);
        }
        for (int k = 0; k < L2_OUT; ++k) {
            for (int j = 0; j < L1_OUT; ++j) {
                stacks[s].w1[k][j] = (j < L1_OUT - 1) ? static_cast<int8_t>(static_cast<int>(next_u64() % 33) - 16) : int8_t(0);
            }
            stacks[s].b1[k] = static_cast<int32_t>(static_cast<int>(next_u64() % 2049) - 1024);
            stacks[s].w2[k] = static_cast<int8_t>(static_cast<int>(next_u64() % 33) - 16);
        }
        stacks[s].b2 = static_cast<int32_t>(static_cast<int>(next_u64() % 2049) - 1024);
    }
    loaded = true;
    id = g_next_net_id.fetch_add(1, std::memory_order_relaxed);
    path = "<random-test>";
}

bool load_global_network(const std::string& path, std::string& err) {
    if (path.empty()) {
        unload_global_network();
        return true;
    }
    auto net = std::make_shared<Network>();
    if (!net->load_file(path, err)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(g_net_mtx);
    g_net = std::move(net);
    return true;
}

void unload_global_network() {
    std::lock_guard<std::mutex> lk(g_net_mtx);
    g_net.reset();
}

std::shared_ptr<const Network> get_global_network() {
    std::lock_guard<std::mutex> lk(g_net_mtx);
    return g_net;
}

void set_nnue_enabled(bool enabled) {
    g_nnue_enabled.store(enabled, std::memory_order_relaxed);
}

bool is_nnue_enabled() {
    return g_nnue_enabled.load(std::memory_order_relaxed);
}

std::string get_global_network_path() {
    std::lock_guard<std::mutex> lk(g_net_mtx);
    return g_net ? g_net->path : std::string();
}

} // namespace homei::nnue

#include "attack.h"
#include "lppn.h"
#include "nnue/nnue.h"
#include "nnue/train_data.h"
#include "types.h"
#include "zobrist.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#ifdef _WIN32
#define CLP_API extern "C" __declspec(dllexport)
#else
#define CLP_API extern "C" __attribute__((visibility("default")))
#endif

namespace {

std::once_flag g_init_once;
void ensure_init() {
    std::call_once(g_init_once, []() {
        homei::init_zobrist();
        homei::init_attack_tables();
    });
}

} // namespace

CLP_API int32_t clp_nnue_constants(int32_t* out_vals) {
    if (!out_vals) return 0;
    out_vals[0]  = homei::nnue::KING_BUCKETS;
    out_vals[1]  = homei::nnue::F_LOCAL;
    out_vals[2]  = homei::nnue::INPUT_SIZE;
    out_vals[3]  = homei::nnue::FT_HALF;
    out_vals[4]  = homei::nnue::NUM_STACKS;
    out_vals[5]  = homei::nnue::L1_OUT;
    out_vals[6]  = homei::nnue::L2_OUT;
    out_vals[7]  = homei::nnue::QA;
    out_vals[8]  = homei::nnue::QW;
    out_vals[9]  = homei::nnue::EVAL_SCALE;
    out_vals[10] = static_cast<int32_t>(sizeof(homei::nnue::PackedSample));
    out_vals[11] = homei::nnue::MAX_ACTIVE_FEATS;
    return 12;
}

CLP_API int32_t clp_decode_batch(
    const homei::nnue::PackedSample* base_samples,
    const int64_t* indices,
    const uint8_t* aug_mirror,
    int32_t batch_size,
    int32_t* stm_global,
    int32_t* stm_local,
    int32_t* stm_offsets,
    int32_t* opp_global,
    int32_t* opp_local,
    int32_t* opp_offsets,
    int32_t* stacks,
    float* scores,
    float* wdls,
    int32_t* out_totals
) {
    if (!base_samples || batch_size <= 0) return 0;

    int32_t stm_pos = 0;
    int32_t opp_pos = 0;

    for (int32_t b = 0; b < batch_size; ++b) {
        const int64_t idx = indices ? indices[b] : static_cast<int64_t>(b);
        const homei::nnue::PackedSample& s = base_samples[idx];
        const bool mirror = aug_mirror ? (aug_mirror[b] != 0) : false;
        const int stm = s.side_to_move & 1;
        const int opp = stm ^ 1;

        stm_offsets[b] = stm_pos;
        opp_offsets[b] = opp_pos;

        int n_stm = homei::nnue::extract_global_features_packed(
            s, stm, mirror, stm_global + stm_pos, stm_local + stm_pos
        );
        int n_opp = homei::nnue::extract_global_features_packed(
            s, opp, mirror, opp_global + opp_pos, opp_local + opp_pos
        );

        stm_pos += n_stm;
        opp_pos += n_opp;

        stacks[b] = homei::nnue::select_stack_packed(s);
        scores[b] = static_cast<float>(s.score);
        wdls[b] = (s.wdl > 0) ? 1.0f : ((s.wdl < 0) ? 0.0f : 0.5f);
    }

    stm_offsets[batch_size] = stm_pos;
    opp_offsets[batch_size] = opp_pos;
    if (out_totals) {
        out_totals[0] = stm_pos;
        out_totals[1] = opp_pos;
    }
    return batch_size;
}

CLP_API int32_t clp_eval_packed_nnue(
    const char* nn_path,
    const homei::nnue::PackedSample* samples,
    int32_t count,
    int32_t* out_cps
) {
    ensure_init();
    if (!nn_path || !samples || !out_cps || count <= 0) return -1;

    homei::nnue::Network net;
    std::string err;
    if (!net.load_file(nn_path, err)) {
        return -2;
    }

    int32_t g_buf[homei::nnue::MAX_ACTIVE_FEATS];
    int32_t l_buf[homei::nnue::MAX_ACTIVE_FEATS];
    homei::nnue::Accumulator acc;
    homei::Position dummy_pos{};

    for (int32_t i = 0; i < count; ++i) {
        const auto& s = samples[i];
        dummy_pos.side_to_move = s.side_to_move & 1U;
        std::memcpy(dummy_pos.prisoners, s.prisoners, sizeof(dummy_pos.prisoners));

        for (int p = 0; p < 2; ++p) {
            std::memcpy(acc.v[p], net.ft_bias, sizeof(net.ft_bias));
            int nf = homei::nnue::extract_global_features_packed(s, p, false, g_buf, l_buf);
            for (int f = 0; f < nf; ++f) {
                const int16_t* row = net.ft_w_aligned + static_cast<size_t>(g_buf[f]) * homei::nnue::FT_HALF;
                for (int k = 0; k < homei::nnue::FT_HALF; ++k) {
                    acc.v[p][k] = static_cast<int16_t>(acc.v[p][k] + row[k]);
                }
            }
            acc.computed[p] = true;
        }
        out_cps[i] = homei::nnue::evaluate_accumulator(net, dummy_pos, acc);
    }
    return count;
}

CLP_API int32_t clp_eval_lppn_nnue(
    const char* nn_path,
    const char* lppn_str,
    int32_t* out_cp
) {
    ensure_init();
    if (!nn_path || !lppn_str || !out_cp) return -1;

    homei::nnue::Network net;
    std::string err;
    if (!net.load_file(nn_path, err)) return -2;

    homei::Position pos;
    int ply = 0;
    if (!homei::parse_lppn(lppn_str, pos, ply, err, true)) return -3;

    *out_cp = homei::nnue::evaluate_position(net, pos);
    return 0;
}

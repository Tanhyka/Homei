#include "search.h"
#include "attack.h"
#include "bitboard.h"
#include "eval.h"
#include "features.h"
#include "geometry.h"
#include "mate.h"
#include "movegen.h"
#include "notation.h"
#include "see.h"
#include "zobrist.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

namespace homei {

namespace {

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct SearchParams {
    int rfp_imp = 77;
    int rfp_noimp = 109;
    int razor_imp = 311;
    int razor_noimp = 267;
    int nmp_eval_div = 188;
    int probcut_margin = 190;
    int futility_imp = 150;
    int futility_noimp = 93;
    int qs_delta_margin = 194;
    int se_margin_x2 = 4;
    int hist_lmr_thresh = 2149;

    static const SearchParams& get() {
        static SearchParams p = init();
        return p;
    }

private:
    static SearchParams init() {
        SearchParams p;
        const char* env = std::getenv("HOMEI_SEARCH_PARAMS");
        if (!env || env[0] == '\0') return p;
        std::string s(env);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t comma = s.find(',', pos);
            std::string tok = (comma == std::string::npos) ? s.substr(pos) : s.substr(pos, comma - pos);
            size_t eq = tok.find('=');
            if (eq != std::string::npos) {
                std::string k = tok.substr(0, eq);
                int v = std::atoi(tok.substr(eq + 1).c_str());
                if (k == "rfp_imp") p.rfp_imp = std::clamp(v, 30, 220);
                else if (k == "rfp_noimp") p.rfp_noimp = std::clamp(v, 40, 260);
                else if (k == "razor_imp") p.razor_imp = std::clamp(v, 100, 600);
                else if (k == "razor_noimp") p.razor_noimp = std::clamp(v, 100, 600);
                else if (k == "nmp_eval_div") p.nmp_eval_div = std::clamp(v, 80, 500);
                else if (k == "probcut_margin") p.probcut_margin = std::clamp(v, 80, 400);
                else if (k == "futility_imp") p.futility_imp = std::clamp(v, 50, 300);
                else if (k == "futility_noimp") p.futility_noimp = std::clamp(v, 40, 260);
                else if (k == "qs_delta_margin") p.qs_delta_margin = std::clamp(v, 60, 400);
                else if (k == "se_margin_x2") p.se_margin_x2 = std::clamp(v, 1, 10);
                else if (k == "hist_lmr_thresh") p.hist_lmr_thresh = std::clamp(v, 500, 10000);
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return p;
    }
};

EngineFeatures g_sfeats{};
SearchParams g_sp{};
std::once_flag g_search_globals_once;

void init_search_globals() {
    std::call_once(g_search_globals_once, []() {
        g_sfeats = EngineFeatures::get();
        g_sp = SearchParams::get();
    });
}

bool move_may_give_check(const Position& pos, const Move& m, int enemy_king) {
    if (enemy_king < 0 || enemy_king >= NUM_SQUARES) return false;
    int end_sq = (m.kind == MoveKind::Rotate) ? m.src : m.dst;
    if (end_sq >= 0 && end_sq < NUM_SQUARES) {
        uint8_t end_f = (m.kind == MoveKind::Drop)
            ? m.facing
            : static_cast<uint8_t>((pos.top_piece[m.src].facing + m.rotate_after) & 3);
        if (piece_attacks_sq(pos, end_sq, m.ptype, end_f, enemy_king)) {
            return true;
        }
    }
    if (m.src >= 0 && m.src < NUM_SQUARES) {
        if (!BB_LINE[m.src][enemy_king].empty()) {
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

bool is_passive_rotate(const Position& pos, const Move& m, Side enemy, int /*enemy_king*/) {
    if (m.kind != MoveKind::Rotate || m.src < 0 || m.src >= NUM_SQUARES) return false;
    if (pos.height[m.src] == 0) return false;
    uint8_t end_f = static_cast<uint8_t>((pos.top_piece[m.src].facing + m.rotate_after) & 3);
    return (piece_attacks_bb(pos, m.src, m.ptype, end_f) & pos.bb_side[enemy]).empty();
}

bool is_bad_koma_quiet(const Position& pos, const Move& m, Side stm) {
    if (m.kind == MoveKind::Drop) return false;
    int end_sq = (m.kind == MoveKind::Rotate) ? m.src : m.dst;
    if (m.src < 0 || m.src >= NUM_SQUARES || end_sq < 0 || end_sq >= NUM_SQUARES) return false;
    if (pos.height[m.src] == 0) return false;

    int r_end = sq_row(end_sq);
    int c_end = sq_col(end_sq);
    int rel_row = (stm == CAM) ? r_end : (8 - r_end);
    uint8_t end_f = static_cast<uint8_t>((pos.top_piece[m.src].facing + m.rotate_after) & 3);

    // 1. In-place rotation of BINH in home territory (rel_row <= 3)
    if (m.kind == MoveKind::Rotate && m.ptype == BINH && rel_row <= 3) {
        return true;
    }

    // 2. Rotating or moving BINH, KI, SI, XA so its facing points directly out of the board
    if (m.ptype == BINH || m.ptype == KI || m.ptype == SI || m.ptype == XA) {
        if (g_sfeats.koma_search_fast) {
            if (get_dead_vec_table()[m.ptype][end_f][end_sq].in_board == 0) {
                return true;
            }
        } else {
            const PieceMoveTable& tbl = get_piece_moves(m.ptype, end_f);
            int in_board_vecs = 0;
            for (int i = 0; i < tbl.count; ++i) {
                if (in_bounds(r_end + tbl.vecs[i].dr, c_end + tbl.vecs[i].dc)) {
                    ++in_board_vecs;
                }
            }
            if (in_board_vecs == 0) {
                return true;
            }
        }
        if (m.ptype == SI && (m.kind == MoveKind::Rotate || m.rotate_after != 0)) {
            int dr_f = (end_f == 0) ? -1 : ((end_f == 2) ? 1 : 0);
            int dc_f = (end_f == 1) ? 1 : ((end_f == 3) ? -1 : 0);
            if (!in_bounds(r_end + dr_f, c_end + dc_f)) {
                return true;
            }
        }
    }
    return false;
}

bool drop_threatens_enemy(const Position& pos, const Move& m, Side enemy, int /*enemy_king*/) {
    if (m.kind != MoveKind::Drop || m.dst < 0 || m.dst >= NUM_SQUARES) return false;
    return (piece_attacks_bb(pos, m.dst, m.ptype, static_cast<uint8_t>(m.facing & 3)) & pos.bb_side[enemy]).any();
}

// Zero-allocation in-place descending sort on [begin, end) by score (keeps fam_ranks in lockstep if non-null).
void sort_move_range_qsort(Move* moves, int* scores, uint8_t* fam_ranks, int lo, int hi) {
    auto swap_idx = [&](int a, int b) {
        std::swap(scores[a], scores[b]);
        std::swap(moves[a], moves[b]);
        if (fam_ranks) std::swap(fam_ranks[a], fam_ranks[b]);
    };
    while (hi - lo > 20) {
        int mid = lo + ((hi - lo) >> 1);
        if (scores[mid] > scores[lo]) swap_idx(mid, lo);
        if (scores[hi - 1] > scores[lo]) swap_idx(hi - 1, lo);
        if (scores[hi - 1] > scores[mid]) swap_idx(hi - 1, mid);
        int pivot = scores[mid];
        int i = lo;
        int j = hi - 1;
        while (true) {
            while (scores[i] > pivot) ++i;
            while (scores[j] < pivot) --j;
            if (i >= j) break;
            swap_idx(i, j);
            ++i;
            --j;
        }
        int split = j + 1;
        if (split - lo < hi - split) {
            sort_move_range_qsort(moves, scores, fam_ranks, lo, split);
            lo = split;
        } else {
            sort_move_range_qsort(moves, scores, fam_ranks, split, hi);
            hi = split;
        }
    }
    for (int i = lo + 1; i < hi; ++i) {
        int sc = scores[i];
        Move mv = moves[i];
        uint8_t fr = fam_ranks ? fam_ranks[i] : 0;
        int j = i - 1;
        while (j >= lo && scores[j] < sc) {
            scores[j + 1] = scores[j];
            moves[j + 1] = moves[j];
            if (fam_ranks) fam_ranks[j + 1] = fam_ranks[j];
            --j;
        }
        scores[j + 1] = sc;
        moves[j + 1] = mv;
        if (fam_ranks) fam_ranks[j + 1] = fr;
    }
}

void sort_move_range(Move* moves, int* scores, int begin, int end, uint8_t* fam_ranks = nullptr) {
    if (end - begin <= 1) return;
    sort_move_range_qsort(moves, scores, fam_ranks, begin, end);
}

// Adaptive selector: partial selection for first few picks, O(K log K) sort for larger remainders.
inline void pick_next_move(Move* moves, int* scores, int cur_idx, int seg_end, int& sorted_until, uint8_t* fam_ranks = nullptr) {
    if (cur_idx < sorted_until) return;
    if (cur_idx >= 8 && (seg_end - cur_idx) > 20) {
        sort_move_range(moves, scores, cur_idx, seg_end, fam_ranks);
        sorted_until = seg_end;
        return;
    }
    int best_idx = cur_idx;
    for (int j = cur_idx + 1; j < seg_end; ++j) {
        if (scores[j] > scores[best_idx]) best_idx = j;
    }
    if (best_idx != cur_idx) {
        std::swap(moves[cur_idx], moves[best_idx]);
        std::swap(scores[cur_idx], scores[best_idx]);
        if (fam_ranks) std::swap(fam_ranks[cur_idx], fam_ranks[best_idx]);
    }
}

constexpr int HIST_MAX = 100000;
constexpr int CONT_HIST_MAX = 16000;
constexpr int CORR_GRAIN = 256;
constexpr int CORR_MAX = 16384;
constexpr int SCORE_SUPERIOR = 25000; // Decisive non-mate cutoff for Superior/Inferior Hand (never triggers fake mate!)

inline void update_hist_entry(int& entry, int bonus, int max_val = HIST_MAX) {
    int clamped = std::clamp(bonus, -max_val, max_val);
    entry += clamped - static_cast<int>((static_cast<int64_t>(entry) * std::abs(clamped)) / max_val);
    entry = std::clamp(entry, -max_val, max_val);
}

inline void update_cont_entry(int16_t& entry, int bonus) {
    int clamped = std::clamp(bonus, -CONT_HIST_MAX, CONT_HIST_MAX);
    int cur = static_cast<int>(entry);
    cur += clamped - (cur * std::abs(clamped)) / CONT_HIST_MAX;
    entry = static_cast<int16_t>(std::clamp(cur, -CONT_HIST_MAX, CONT_HIST_MAX));
}

inline uint64_t mix64_fast(uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    return x;
}

inline uint64_t hash_bb_pair(const Bitboard& a, const Bitboard& b) {
    uint64_t h = a.lo ^ (a.hi * 0x9E3779B97F4A7C15ULL);
    h ^= (b.lo * 0xBF58476D1CE4E5B9ULL) ^ (b.hi * 0x94D049BB133111EBULL);
    return mix64_fast(h);
}

// Packs prisoners[s][1..7] into 7 six-bit lanes (5-bit count 0..31 + 1-bit zero guard) for 3-instruction SWAR >= comparison.
constexpr uint64_t HAND_GUARD_MASK = 0x0000020820820820ULL;

inline uint64_t pack_hand_u64(const Position& pos, Side s) {
    uint64_t h = 0ULL;
    for (int pt = 1; pt < 8; ++pt) {
        h |= (static_cast<uint64_t>(pos.prisoners[s][pt] & 31U) << ((pt - 1) * 6));
    }
    return h;
}

inline bool hand_ge_swar(uint64_t a, uint64_t b) {
    return ((((a | HAND_GUARD_MASK) - b) & HAND_GUARD_MASK) == HAND_GUARD_MASK);
}

inline int facing_quality_score_with_att(
    int sq, uint8_t pt, uint8_t f, Side stm, Side enemy, int enemy_king, int ekr, int ekc,
    const Bitboard& att, const Bitboard& enemy_bb
) {
    int q = 0;
    int r = sq_row(sq);
    int c = sq_col(sq);
    uint8_t fwd = initial_facing(stm);
    uint8_t bwd = initial_facing(enemy);
    if (f == fwd) q += 40;
    else if (f == bwd) q -= 30;
    else q += 10;

    if (pt == BINH || pt == KI || pt == SI || pt == XA) {
        const DeadVecInfo& dvi = get_dead_vec_table()[pt][f][sq];
        if (dvi.in_board == 0) q -= 200;
        q -= dvi.out_board * 25;
    }
    if (ekr >= 0) {
        if (att.test_bit(enemy_king)) q += 300;
        if ((f == 0 && ekr < r) || (f == 2 && ekr > r) || (f == 1 && ekc > c) || (f == 3 && ekc < c)) {
            q += 60;
        }
    }
    if ((att & enemy_bb).any()) {
        q += 120;
    }
    return q;
}

int lmr_table_1024[64][128]{};
std::once_flag lmr_table_once;

void init_lmr_table() {
    std::call_once(lmr_table_once, []() {
        for (int d = 1; d < 64; ++d) {
            for (int m = 1; m < 128; ++m) {
                double v = 0.50 + (std::log(static_cast<double>(d)) * std::log(static_cast<double>(m))) / 2.28;
                lmr_table_1024[d][m] = static_cast<int>(std::round(v * 1024.0));
            }
        }
    });
}

} // namespace

Searcher::Searcher(bool is_worker)
    : tt_(is_worker ? 0 : 64),
      tt_ptr_(is_worker ? nullptr : &tt_),
      is_worker_(is_worker),
      ply_stack_(MAX_PLY + 8),
      cont_hist_1ply_(static_cast<size_t>(CONT_PF_SQ) * CONT_PF_SQ, 0),
      cont_hist_2ply_(static_cast<size_t>(CONT_PT_SQ) * CONT_PF_SQ, 0),
      cont_hist_4ply_(static_cast<size_t>(CONT_PT_SQ) * CONT_PT_SQ, 0),
      nnue_stack_(MAX_PLY + 8) {
    init_zobrist();
    init_attack_tables();
    init_lmr_table();
    init_search_globals();
    (void)get_eval_params();
    (void)get_dead_vec_table();
    clear_ordering_tables();
}

int Searcher::eval_node(const Position& pos, int ply, int alpha, int beta, bool allow_lazy, bool& out_is_lazy) {
    if (active_nnue_) {
        out_is_lazy = false;
        const nnue::Network& net = *active_nnue_;
        nnue::PlyAccNode& node = nnue_stack_[ply];
        nnue::FinnyTable* finny = nnue_finny_.get();
        for (int p = 0; p < 2; ++p) {
            if (!node.acc.computed[p]) {
                int anc = ply - 1;
                while (anc >= 0 && !nnue_stack_[anc].acc.computed[p]) {
                    --anc;
                }
                if (anc >= 0) {
                    nnue::update_perspective(net, pos, p, nnue_stack_[anc], node, finny);
                } else {
                    nnue::refresh_perspective(net, pos, p, node, finny);
                }
            }
        }
        return nnue::evaluate_accumulator(net, pos, node.acc);
    }
    return evaluate_maybe_lazy(pos, alpha, beta, allow_lazy, out_is_lazy);
}

Searcher::~Searcher() {
    if (is_worker_) {
        stop_worker_loop();
    } else {
        for (auto& w : workers_) {
            if (w) w->stop_worker_loop();
        }
        workers_.clear();
    }
}

void Searcher::start_worker_loop(int thread_id) {
    if (!is_worker_ || worker_thread_.joinable()) return;
    thread_id_ = thread_id;
    worker_exit_ = false;
    worker_has_job_ = false;
    worker_busy_ = false;
    worker_thread_ = std::thread([this]() {
        while (true) {
            Position rpos;
            SearchLimits lim;
            std::atomic<bool>* stop_ptr = nullptr;
            bool do_age = false;
            int rply = 0;
            {
                std::unique_lock<std::mutex> lk(worker_mtx_);
                worker_cv_.wait(lk, [this]() { return worker_exit_ || worker_has_job_; });
                if (worker_exit_) break;
                rpos = job_root_pos_;
                lim = job_limits_;
                stop_ptr = job_stop_;
                do_age = job_age_tables_;
                rply = job_root_ply_;
                worker_has_job_ = false;
                worker_busy_ = true;
            }
            root_ply_ = rply;
            if (do_age) age_ordering_tables();
            else clear_ordering_tables();

            search_worker(rpos, lim, *stop_ptr, thread_id_);

            {
                std::lock_guard<std::mutex> lk(worker_mtx_);
                worker_busy_ = false;
            }
            worker_done_cv_.notify_all();
        }
    });
}

void Searcher::stop_worker_loop() {
    if (!worker_thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(worker_mtx_);
        worker_exit_ = true;
    }
    worker_cv_.notify_all();
    worker_thread_.join();
}

void Searcher::dispatch_job(
    const Position& root_pos,
    const SearchLimits& limits,
    std::atomic<bool>& stop_flag,
    const std::vector<BaseRepEntry>& br,
    const std::vector<RepHashEntry>& mr,
    int root_ply,
    bool age_tables
) {
    {
        std::lock_guard<std::mutex> lk(worker_mtx_);
        set_reps(br, mr);
        job_root_pos_ = root_pos;
        job_limits_ = limits;
        job_stop_ = &stop_flag;
        job_root_ply_ = root_ply;
        job_age_tables_ = age_tables;
        voted_depth_ = 0;
        voted_score_ = 0;
        voted_move_ = Move::none();
        voted_pv_.clear();
        nodes_ = 0;
        live_nodes_atomic_.store(0, std::memory_order_relaxed);
        worker_has_job_ = true;
    }
    worker_cv_.notify_one();
}

void Searcher::wait_job_done() {
    std::unique_lock<std::mutex> lk(worker_mtx_);
    worker_done_cv_.wait(lk, [this]() { return !worker_has_job_ && !worker_busy_; });
}

void Searcher::ensure_workers() {
    if (is_worker_) return;
    size_t target = (num_threads_ > 1) ? static_cast<size_t>(num_threads_ - 1) : 0;
    while (workers_.size() > target) {
        workers_.back()->stop_worker_loop();
        workers_.pop_back();
    }
    while (workers_.size() < target) {
        auto w = std::make_unique<Searcher>(true);
        w->set_shared_tt(tt_ptr_);
        int tid = static_cast<int>(workers_.size() + 1);
        w->start_worker_loop(tid);
        workers_.push_back(std::move(w));
    }
}

void Searcher::clear_ordering_tables() {
    std::memset(killers_, 0, sizeof(killers_));
    std::memset(history_, 0, sizeof(history_));
    std::memset(drop_history_, 0, sizeof(drop_history_));
    std::memset(cap_history_, 0, sizeof(cap_history_));
    std::memset(countermoves_, 0, sizeof(countermoves_));
    std::memset(corr_binh_, 0, sizeof(corr_binh_));
    std::memset(corr_minor_, 0, sizeof(corr_minor_));
    std::memset(corr_non_binh_, 0, sizeof(corr_non_binh_));
    std::memset(corr_cont2_, 0, sizeof(corr_cont2_));
    std::memset(corr_cont46_, 0, sizeof(corr_cont46_));
    std::fill(cont_hist_1ply_.begin(), cont_hist_1ply_.end(), int16_t(0));
    std::fill(cont_hist_2ply_.begin(), cont_hist_2ply_.end(), int16_t(0));
    std::fill(cont_hist_4ply_.begin(), cont_hist_4ply_.end(), int16_t(0));
}

void Searcher::age_ordering_tables() {
    std::memset(killers_, 0, sizeof(killers_));
    for (int s = 0; s < 2; ++s) {
        for (int pt = 0; pt < 8; ++pt) {
            for (int f = 0; f < 4; ++f) {
                for (int sq = 0; sq < NUM_SQUARES; ++sq) {
                    history_[s][pt][f][sq] /= 2;
                    drop_history_[s][pt][f][sq] /= 2;
                }
            }
            for (int vpt = 0; vpt < 8; ++vpt) {
                for (int sq = 0; sq < NUM_SQUARES; ++sq) {
                    cap_history_[s][pt][vpt][sq] /= 2;
                }
            }
        }
        for (int i = 0; i < CORR_SIZE; ++i) {
            corr_binh_[s][i] = static_cast<int16_t>((corr_binh_[s][i] * 3) / 4);
            corr_minor_[s][i] = static_cast<int16_t>((corr_minor_[s][i] * 3) / 4);
            corr_non_binh_[s][0][i] = static_cast<int16_t>((corr_non_binh_[s][0][i] * 3) / 4);
            corr_non_binh_[s][1][i] = static_cast<int16_t>((corr_non_binh_[s][1][i] * 3) / 4);
            corr_cont2_[s][i] = static_cast<int16_t>((corr_cont2_[s][i] * 3) / 4);
            corr_cont46_[s][i] = static_cast<int16_t>((corr_cont46_[s][i] * 3) / 4);
        }
    }
    // Continuation history uses gravity clamping (self-decaying) and is preserved across consecutive plies
    // without thrashing 7.34 MB of L3 cache before every move (cleared cleanly on new games/positions).
}

void Searcher::record_path_node(const Position& pos, int ply) {
    path_hashes_[ply] = pos.zobrist;
    path_board_keys_[ply] = pos.board_key;
    path_hand_keys_[ply] = pos.hand_key;
    path_hand_packed_[ply][CAM] = pack_hand_u64(pos, CAM);
    path_hand_packed_[ply][XANH] = pack_hand_u64(pos, XANH);
    path_reserve_[ply][CAM] = pos.reserve[CAM];
    path_reserve_[ply][XANH] = pos.reserve[XANH];
}

int Searcher::check_superior_inferior_hand(const Position& pos, int ply) const {
    if (ply < 2) return 0;
    const uint64_t bk = pos.board_key;
    const uint64_t hk = pos.hand_key;
    const int min_p = std::max(0, ply - 16);
    for (int p = ply - 2; p >= min_p; p -= 2) {
        if (path_hashes_[p + 1] == 0ULL || path_hashes_[p] == 0ULL) break;
        if (path_board_keys_[p] == bk && path_hand_keys_[p] != hk) {
            const Side stm = static_cast<Side>(pos.side_to_move);
            const Side enemy = other_side(stm);
            if (pos.reserve[stm] != path_reserve_[p][stm] || pos.reserve[enemy] != path_reserve_[p][enemy]) {
                continue;
            }
            const uint64_t cur_s = path_hand_packed_[ply][stm];
            const uint64_t cur_e = path_hand_packed_[ply][enemy];
            const uint64_t prv_s = path_hand_packed_[p][stm];
            const uint64_t prv_e = path_hand_packed_[p][enemy];
            if (cur_s != prv_s || cur_e != prv_e) {
                if (hand_ge_swar(cur_s, prv_s) && hand_ge_swar(prv_e, cur_e)) return 1;
                if (hand_ge_swar(prv_s, cur_s) && hand_ge_swar(cur_e, prv_e)) return -1;
            }
        }
    }
    return 0;
}

int Searcher::correction_delta(const Position& pos, int ply) const {
    const Side stm = static_cast<Side>(pos.side_to_move);
    const Bitboard bb_b = pos.bb_type[BINH];
    const uint64_t bh = hash_bb_pair(bb_b & pos.bb_side[CAM], bb_b & pos.bb_side[XANH]);
    const Bitboard bb_m = pos.bb_type[TUONG] | pos.bb_type[SI] | pos.bb_type[KI] | pos.bb_type[XA];
    const uint64_t mh = hash_bb_pair(bb_m & pos.bb_side[CAM], bb_m & pos.bb_side[XANH]);
    const Bitboard nb_cam = pos.bb_side[CAM] & ~bb_b;
    const Bitboard nb_xanh = pos.bb_side[XANH] & ~bb_b;
    const uint64_t nch = mix64_fast(nb_cam.lo ^ (nb_cam.hi * 0x9E3779B97F4A7C15ULL) ^ pos.hand_key);
    const uint64_t nxh = mix64_fast(nb_xanh.lo ^ (nb_xanh.hi * 0x9E3779B97F4A7C15ULL) ^ (pos.hand_key * 0xBF58476D1CE4E5B9ULL));

    int sum = static_cast<int>(corr_binh_[stm][bh & CORR_MASK]) * 3
            + static_cast<int>(corr_minor_[stm][mh & CORR_MASK]) * 2
            + (static_cast<int>(corr_non_binh_[stm][CAM][nch & CORR_MASK])
             + static_cast<int>(corr_non_binh_[stm][XANH][nxh & CORR_MASK])) * 2;

    if (ply >= 2 && move_ptsq_stack_[ply - 1] >= 0 && move_ptsq_stack_[ply - 2] >= 0) {
        uint64_t c2k = mix64_fast((static_cast<uint64_t>(move_ptsq_stack_[ply - 1]) << 11) ^ static_cast<uint64_t>(move_ptsq_stack_[ply - 2]));
        sum += static_cast<int>(corr_cont2_[stm][c2k & CORR_MASK]) * 2;
    }
    if (ply >= 4 && move_ptsq_stack_[ply - 1] >= 0 && move_ptsq_stack_[ply - 4] >= 0) {
        uint64_t p6 = (ply >= 6 && move_ptsq_stack_[ply - 6] >= 0) ? static_cast<uint64_t>(move_ptsq_stack_[ply - 6]) : 0ULL;
        uint64_t c46k = mix64_fast((static_cast<uint64_t>(move_ptsq_stack_[ply - 1]) << 20) ^
                                   (static_cast<uint64_t>(move_ptsq_stack_[ply - 4]) << 10) ^ p6);
        sum += static_cast<int>(corr_cont46_[stm][c46k & CORR_MASK]);
    }
    return std::clamp(sum / (CORR_GRAIN * 4), -120, 120);
}

void Searcher::update_correction_history(const Position& pos, int ply, int depth, int diff) {
    const Side stm = static_cast<Side>(pos.side_to_move);
    const int bonus = std::clamp((diff * depth * CORR_GRAIN) / 96, -CORR_MAX / 6, CORR_MAX / 6);
    if (bonus == 0) return;
    auto upd = [&](int16_t& entry) {
        int cur = static_cast<int>(entry);
        cur += bonus - (cur * std::abs(bonus)) / CORR_MAX;
        entry = static_cast<int16_t>(std::clamp(cur, -CORR_MAX, CORR_MAX));
    };

    const Bitboard bb_b = pos.bb_type[BINH];
    const uint64_t bh = hash_bb_pair(bb_b & pos.bb_side[CAM], bb_b & pos.bb_side[XANH]);
    const Bitboard bb_m = pos.bb_type[TUONG] | pos.bb_type[SI] | pos.bb_type[KI] | pos.bb_type[XA];
    const uint64_t mh = hash_bb_pair(bb_m & pos.bb_side[CAM], bb_m & pos.bb_side[XANH]);
    const Bitboard nb_cam = pos.bb_side[CAM] & ~bb_b;
    const Bitboard nb_xanh = pos.bb_side[XANH] & ~bb_b;
    const uint64_t nch = mix64_fast(nb_cam.lo ^ (nb_cam.hi * 0x9E3779B97F4A7C15ULL) ^ pos.hand_key);
    const uint64_t nxh = mix64_fast(nb_xanh.lo ^ (nb_xanh.hi * 0x9E3779B97F4A7C15ULL) ^ (pos.hand_key * 0xBF58476D1CE4E5B9ULL));

    upd(corr_binh_[stm][bh & CORR_MASK]);
    upd(corr_minor_[stm][mh & CORR_MASK]);
    upd(corr_non_binh_[stm][CAM][nch & CORR_MASK]);
    upd(corr_non_binh_[stm][XANH][nxh & CORR_MASK]);

    if (ply >= 2 && move_ptsq_stack_[ply - 1] >= 0 && move_ptsq_stack_[ply - 2] >= 0) {
        uint64_t c2k = mix64_fast((static_cast<uint64_t>(move_ptsq_stack_[ply - 1]) << 11) ^ static_cast<uint64_t>(move_ptsq_stack_[ply - 2]));
        upd(corr_cont2_[stm][c2k & CORR_MASK]);
    }
    if (ply >= 4 && move_ptsq_stack_[ply - 1] >= 0 && move_ptsq_stack_[ply - 4] >= 0) {
        uint64_t p6 = (ply >= 6 && move_ptsq_stack_[ply - 6] >= 0) ? static_cast<uint64_t>(move_ptsq_stack_[ply - 6]) : 0ULL;
        uint64_t c46k = mix64_fast((static_cast<uint64_t>(move_ptsq_stack_[ply - 1]) << 20) ^
                                   (static_cast<uint64_t>(move_ptsq_stack_[ply - 4]) << 10) ^ p6);
        upd(corr_cont46_[stm][c46k & CORR_MASK]);
    }
}

int Searcher::get_5ply_cont_hist(int ply, uint8_t pt, int8_t end_f, int end_sq) const {
    if (end_sq < 0 || end_sq >= NUM_SQUARES) return 0;
    const int curr_pfsq = pfsq_idx(pt, end_f, end_sq);
    const int curr_ptsq = ptsq_idx(pt, end_sq);
    int total = 0;
    if (ply >= 1 && move_pfsq_stack_[ply - 1] >= 0) {
        total += static_cast<int>(cont_hist_1ply_[static_cast<size_t>(move_pfsq_stack_[ply - 1]) * CONT_PF_SQ + curr_pfsq]);
    }
    if (ply >= 2 && move_ptsq_stack_[ply - 2] >= 0) {
        total += static_cast<int>(cont_hist_2ply_[static_cast<size_t>(move_ptsq_stack_[ply - 2]) * CONT_PF_SQ + curr_pfsq]);
    }
    if (ply >= 4 && move_ptsq_stack_[ply - 4] >= 0) {
        total += static_cast<int>(cont_hist_4ply_[static_cast<size_t>(move_ptsq_stack_[ply - 4]) * CONT_PT_SQ + curr_ptsq]) / 2;
    }
    return total;
}

void Searcher::update_5ply_cont_hist(int ply, uint8_t pt, int8_t end_f, int end_sq, int bonus) {
    if (end_sq < 0 || end_sq >= NUM_SQUARES) return;
    const int curr_pfsq = pfsq_idx(pt, end_f, end_sq);
    const int curr_ptsq = ptsq_idx(pt, end_sq);
    if (ply >= 1 && move_pfsq_stack_[ply - 1] >= 0) {
        update_cont_entry(cont_hist_1ply_[static_cast<size_t>(move_pfsq_stack_[ply - 1]) * CONT_PF_SQ + curr_pfsq], bonus);
    }
    if (ply >= 2 && move_ptsq_stack_[ply - 2] >= 0) {
        update_cont_entry(cont_hist_2ply_[static_cast<size_t>(move_ptsq_stack_[ply - 2]) * CONT_PF_SQ + curr_pfsq], bonus);
    }
    if (ply >= 4 && move_ptsq_stack_[ply - 4] >= 0) {
        update_cont_entry(cont_hist_4ply_[static_cast<size_t>(move_ptsq_stack_[ply - 4]) * CONT_PT_SQ + curr_ptsq], (bonus * 3) / 4);
    }
}

bool Searcher::check_stop() {
    if ((nodes_ & 255ULL) == 0) {
        live_nodes_atomic_.store(nodes_, std::memory_order_relaxed);
    }
    if (abort_) return true;
    const bool have_move = current_root_depth_ > 1 || root_has_result_;
    if (have_move && !limits_.infinite && nodes_ >= limits_.max_nodes) {
        abort_ = true;
        return true;
    }
    if ((nodes_ & 255ULL) == 0) {
        if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) {
            if (is_worker_ || have_move) {
                abort_ = true;
                return true;
            }
        }
    }
    if (!limits_.infinite && (nodes_ & 1023ULL) == 0) {
        int64_t elapsed = now_ms() - start_time_ms_;
        if ((have_move && elapsed >= hard_limit_ms_) ||
            (elapsed >= hard_limit_ms_ + 500)) {
            abort_ = true;
            return true;
        }
    }
    return false;
}

int Searcher::count_repetitions(uint64_t hash, int ply) const {
    if (hash == 0ULL) return 1;
    int count = 0;
    const uint64_t bh = hash ^ (hash >> 29);
    if (base_rep_bloom_[(bh >> 6) & 3] & (1ULL << (bh & 63))) {
        for (const auto& e : base_rep_) {
            if (e.hash == hash) {
                count += e.count;
            }
        }
    }
    // Check search path after root (ply 1 .. ply - 1), ignoring sentinel 0ULL (null-move)
    for (int i = 1; i < ply; ++i) {
        if (path_hashes_[i] == hash && path_hashes_[i] != 0ULL) {
            ++count;
        }
    }
    // Plus the current occurrence at `ply` (when ply >= 1)
    if (ply >= 1) {
        ++count;
    }
    return count;
}

bool Searcher::has_any_non_rep3_move(Position& pos, int ply) const {
    MoveList all_legal;
    generate_legal_moves(pos, all_legal);
    UndoState u;
    for (int i = 0; i < all_legal.count; ++i) {
        apply_move(pos, all_legal.moves[i], u);
        int rep = count_repetitions(pos.zobrist, ply + 1);
        undo_move(pos, all_legal.moves[i], u);
        if (rep < 3) {
            return true;
        }
    }
    return false;
}

void Searcher::score_moves(
    Position& pos, const MoveList& list, int* scores, Move tt_move, int ply,
    Move counter_move, Move prev1_move, Move prev2_move, int8_t prev1_f, int8_t /*prev2_f*/,
    bool quiets_only, uint8_t* out_fam_rank
) const {
#ifdef HOMEI_PROFILE
    SearchProfile& sprof = get_search_profile();
    const bool sprof_on = sprof.enabled;
    std::chrono::steady_clock::time_point ts0;
    if (sprof_on) {
        ++sprof.score_moves_calls;
        ts0 = std::chrono::steady_clock::now();
    }
    struct SProfGuard {
        bool on;
        std::chrono::steady_clock::time_point t0;
        ~SProfGuard() {
            if (on) {
                get_search_profile().score_moves_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()
                );
            }
        }
    } sguard{sprof_on, ts0};
#endif
    const Side stm = static_cast<Side>(pos.side_to_move);
    const Side enemy = other_side(stm);
    const int enemy_king = pos.king_sq[enemy];
    const int ekr = (enemy_king < NUM_SQUARES) ? sq_row(enemy_king) : -1;
    const int ekc = (enemy_king < NUM_SQUARES) ? sq_col(enemy_king) : -1;
    const int own_king = pos.king_sq[stm];
    const int okr = (own_king < NUM_SQUARES) ? sq_row(own_king) : -1;
    const int okc = (own_king < NUM_SQUARES) ? sq_col(own_king) : -1;
    const Bitboard enemy_bb = pos.bb_side[enemy];

    const bool drop_hist_on = g_sfeats.drop_history;
    const bool koma_on = g_sfeats.koma_search || g_sfeats.koma_search_fast;
    const bool koma_fast = g_sfeats.koma_search_fast;

    // Precompute facing-aware 1/2/4-ply Thread-Local Continuation History base pointers ONCE per node (zero atomic_ref!)
    int p1_pfsq = (ply >= 1) ? move_pfsq_stack_[ply - 1] : -1;
    if (p1_pfsq < 0 && !prev1_move.is_none()) {
        int sq = (prev1_move.kind == MoveKind::Rotate) ? prev1_move.src : prev1_move.dst;
        if (sq >= 0 && sq < NUM_SQUARES) p1_pfsq = pfsq_idx(prev1_move.ptype, prev1_f, sq);
    }
    int p2_ptsq = (ply >= 2) ? move_ptsq_stack_[ply - 2] : -1;
    if (p2_ptsq < 0 && !prev2_move.is_none()) {
        int sq = (prev2_move.kind == MoveKind::Rotate) ? prev2_move.src : prev2_move.dst;
        if (sq >= 0 && sq < NUM_SQUARES) p2_ptsq = ptsq_idx(prev2_move.ptype, sq);
    }
    const int p4_ptsq = (ply >= 4) ? move_ptsq_stack_[ply - 4] : -1;

    const int16_t* ch1_base = (p1_pfsq >= 0) ? (cont_hist_1ply_.data() + static_cast<size_t>(p1_pfsq) * CONT_PF_SQ) : nullptr;
    const int16_t* ch2_base = (p2_ptsq >= 0) ? (cont_hist_2ply_.data() + static_cast<size_t>(p2_ptsq) * CONT_PF_SQ) : nullptr;
    const int16_t* ch4_base = (p4_ptsq >= 0) ? (cont_hist_4ply_.data() + static_cast<size_t>(p4_ptsq) * CONT_PT_SQ) : nullptr;

    auto read_5ply_ch = [&](uint8_t pt, int8_t f, int sq) -> int {
        int sum = 0;
        if (ch1_base || ch2_base) {
            int pfsq = pfsq_idx(pt, f, sq);
            if (ch1_base) sum += static_cast<int>(ch1_base[pfsq]);
            if (ch2_base) sum += static_cast<int>(ch2_base[pfsq]);
        }
        if (ch4_base) {
            sum += static_cast<int>(ch4_base[ptsq_idx(pt, sq)]) / 2;
        }
        return sum;
    };

    // Fast single-pass bitboard attack map when ply <= 2
    Bitboard att_bb[2]{};
    int src_danger[NUM_SQUARES]{};
    int danger_sqs[NUM_SQUARES];
    int danger_count = 0;

    if (ply <= 2) {
        Bitboard occ = pos.bb_occ;
        while (occ.any()) {
            int sq = occ.pop_lsb();
            Side owner = pos.cell_owner(sq);
            const Piece top = pos.top_piece[sq];
            att_bb[owner] |= piece_attacks_bb(pos, sq, top.type, top.facing);
        }
        Bitboard danger_bb = pos.bb_side[stm] & att_bb[enemy];
        if (own_king < NUM_SQUARES) danger_bb.clear_bit(own_king);
        while (danger_bb.any()) {
            int sq = danger_bb.pop_lsb();
            if (pos.cell_contains_king(sq)) continue;
            int loss = 0;
            if (ply <= 1) {
#ifdef HOMEI_PROFILE
                SearchProfile& prof = get_search_profile();
                if (prof.enabled) {
                    ++prof.hanging_calls;
                    auto th0 = std::chrono::steady_clock::now();
                    loss = square_hanging_loss_fast(pos, sq, att_bb);
                    auto th1 = std::chrono::steady_clock::now();
                    prof.hanging_ns += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(th1 - th0).count()
                    );
                } else {
                    loss = square_hanging_loss_fast(pos, sq, att_bb);
                }
#else
                loss = square_hanging_loss_fast(pos, sq, att_bb);
#endif
            } else {
                loss = cell_material_value(pos, sq);
            }
            if (loss > 0) {
                src_danger[sq] = loss;
                danger_sqs[danger_count++] = sq;
            }
        }
    }

    // Cache is_attacked(pos, m.src, enemy) per src square across all moves from the same piece!
    Bitboard src_att_known = (ply <= 2) ? pos.bb_side[stm] : Bitboard{};
    Bitboard src_att_val   = (ply <= 2) ? att_bb[enemy] : Bitboard{};
    auto is_src_attacked = [&](int ssq) -> bool {
        if (ssq < 0) return false;
        if (!src_att_known.test_bit(ssq)) {
            src_att_known.set_bit(ssq);
            if (is_attacked(pos, ssq, enemy)) src_att_val.set_bit(ssq);
        }
        return src_att_val.test_bit(ssq);
    };

    // Contiguous run-length cache for facing quality & Move-Family rank (0=best, 1=2nd, 2=3rd, 3=4th)
    int fq_run_lo = -1, fq_run_hi = -1;
    int fq_best = 0;
    int fq_vals[4]{};
    uint8_t fq_ranks[4]{};

    auto eval_fq_run = [&](int i, bool is_drop, uint8_t& out_rank) -> int {
        if (i >= fq_run_lo && i < fq_run_hi) {
            out_rank = fq_ranks[i - fq_run_lo];
            return fq_vals[i - fq_run_lo];
        }
        const Move& m0 = list.moves[i];
        int lo = i;
        int hi = i + 1;
        if (is_drop) {
            while (lo > 0 && (hi - lo) < 4) {
                const Move& pm = list.moves[lo - 1];
                if (pm.kind == MoveKind::Drop && pm.ptype == m0.ptype && pm.dst == m0.dst) --lo;
                else break;
            }
            while (hi < list.count && (hi - lo) < 4) {
                const Move& nm = list.moves[hi];
                if (nm.kind == MoveKind::Drop && nm.ptype == m0.ptype && nm.dst == m0.dst) ++hi;
                else break;
            }
        } else {
            while (lo > 0 && (hi - lo) < 4) {
                const Move& pm = list.moves[lo - 1];
                if (pm.kind == m0.kind && pm.src == m0.src && pm.dst == m0.dst && pm.ptype == m0.ptype) --lo;
                else break;
            }
            while (hi < list.count && (hi - lo) < 4) {
                const Move& nm = list.moves[hi];
                if (nm.kind == m0.kind && nm.src == m0.src && nm.dst == m0.dst && nm.ptype == m0.ptype) ++hi;
                else break;
            }
        }
        const int dsq = m0.dst;
        const uint8_t pt = m0.ptype;
        const Bitboard xa_ortho = (pt == XA) ? ortho_attacks(dsq, pos.bb_occ) : Bitboard{};
        const uint8_t base_src_f = is_drop ? 0 : pos.top_piece[m0.src].facing;
        int best_q = -1000000;
        const int len = hi - lo;
        for (int k = 0; k < len; ++k) {
            const Move& km = list.moves[lo + k];
            uint8_t ef = is_drop ? static_cast<uint8_t>(km.facing & 3)
                                 : static_cast<uint8_t>((base_src_f + km.rotate_after) & 3);
            Bitboard att = (pt == XA) ? (xa_ortho & BB_RAY_ORTHO[dsq][ef]) : BB_STEP_ATTACKS[pt][ef][dsq];
            int q = facing_quality_score_with_att(dsq, pt, ef, stm, enemy, enemy_king, ekr, ekc, att, enemy_bb);
            fq_vals[k] = q;
            if (q > best_q) best_q = q;
        }
        // Compute strict rank 0..3 within this move family (higher fq = lower rank; ties share the same rank!)
        for (int a = 0; a < len; ++a) {
            uint8_t rk = 0;
            for (int b = 0; b < len; ++b) {
                if (fq_vals[b] > fq_vals[a]) ++rk;
            }
            fq_ranks[a] = rk;
        }
        fq_run_lo = lo;
        fq_run_hi = hi;
        fq_best = best_q;
        out_rank = fq_ranks[i - lo];
        return fq_vals[i - lo];
    };

    // Cache SEE across consecutive rotate_after (0, 1, 3) for the same capture (kind, src, dst)
    MoveKind last_see_kind = MoveKind::Rotate;
    int8_t last_see_src = -2;
    int8_t last_see_dst = -2;
    int last_see_gain = 0;

    for (int i = 0; i < list.count; ++i) {
        const Move& m = list.moves[i];
        uint8_t frank = 0;
        if (!quiets_only && !tt_move.is_none() && m == tt_move) {
            scores[i] = 2000000;
            if (out_fam_rank) out_fam_rank[i] = 0;
            continue;
        }

        int sc = 0;
        if (!quiets_only && is_capture_move(pos, m)) {
            int v_dst = cell_material_value(pos, m.dst);
            int v_att = (m.kind == MoveKind::Team)
                ? cell_material_value(pos, m.src)
                : piece_material_value(pos.top_piece[m.src].type);
            uint8_t vic_pt = pos.top_piece[m.dst].type;
            int chist = (m.dst >= 0 && m.dst < NUM_SQUARES) ? cap_history_[stm][m.ptype][vic_pt][m.dst] : 0;
            int see_gain;
            if (m.kind == last_see_kind && m.src == last_see_src && m.dst == last_see_dst) {
                see_gain = last_see_gain;
            } else {
                see_gain = stack_see_estimate(pos, m);
                last_see_kind = m.kind;
                last_see_src = m.src;
                last_see_dst = m.dst;
                last_see_gain = see_gain;
            }

            int follow_bonus = 0;
            if (ply <= 2 && can_rotate(m.ptype)) {
                uint8_t eff_f = static_cast<uint8_t>((pos.top_piece[m.src].facing + m.rotate_after) & 3);
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
                            if (tsq != m.src && pos.height[tsq] > 0 && pos.cell_owner(tsq) == enemy) {
                                follow_bonus += 16 + cell_material_value(pos, tsq) / 32;
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
                sc = 1000000 + see_gain * 64 + (10 * v_dst - v_att) + chist / 4 + follow_bonus;
            } else {
                sc = -100000 + see_gain * 64 + (10 * v_dst - v_att) + chist / 4 + follow_bonus;
            }
        } else if (m.kind == MoveKind::Drop) {
            int v_drop = piece_material_value(m.ptype);
            int r = sq_row(m.dst);
            int c = sq_col(m.dst);
            uint8_t drop_f = static_cast<uint8_t>(m.facing & 3);
            int dist_ek = (ekr >= 0) ? (std::abs(r - ekr) + std::abs(c - ekc)) : 8;
            sc = 1000 + v_drop + (16 - dist_ek) * 20;

            if (drop_hist_on) {
                if (!quiets_only && ply < MAX_PLY) {
                    if (m == killers_[ply][0]) sc = std::max(sc, 780000);
                    else if (m == killers_[ply][1]) sc = std::max(sc, 730000);
                }
                if (m.dst >= 0 && m.dst < NUM_SQUARES) {
                    sc += std::clamp(drop_history_[stm][m.ptype][drop_f][m.dst] / 2, -80000, 80000);
                    sc += read_5ply_ch(m.ptype, static_cast<int8_t>(drop_f), m.dst);
                }
            }

            if (!quiets_only && !counter_move.is_none() && m == counter_move) {
                sc = std::max(sc, 700000);
            }

            bool is_tactical_drop = false;
            if (ply <= 1 && okr >= 0 && pos.height[m.dst] == 0 &&
                std::abs(r - okr) <= 1 && std::abs(c - okc) <= 1) {
                if (att_bb[enemy].test_bit(m.dst) && att_bb[stm].test_bit(m.dst)) {
                    sc += 300000 - v_drop * 16;
                    is_tactical_drop = true;
                }
            }

            if (enemy_king < NUM_SQUARES && piece_attacks_sq(pos, m.dst, m.ptype, drop_f, enemy_king)) {
                sc += 380000 - v_drop * 4;
                is_tactical_drop = true;
            }

            if (!is_tactical_drop && ply <= 2 && (!att_bb[enemy].test_bit(m.dst) || att_bb[stm].test_bit(m.dst))) {
                Bitboard enemy_non_king = enemy_bb;
                if (enemy_king < NUM_SQUARES) enemy_non_king.clear_bit(enemy_king);
                Bitboard hit_bb = piece_attacks_bb(pos, m.dst, m.ptype, drop_f) & enemy_non_king;
                int attacked_targets = 0;
                int attacked_value_sum = 0;
                while (hit_bb.any()) {
                    int tsq = hit_bb.pop_lsb();
                    if (pos.cell_contains_king(tsq)) continue;
                    int tval = cell_material_value(pos, tsq);
                    if (!att_bb[enemy].test_bit(tsq) || tval > v_drop) {
                        ++attacked_targets;
                        attacked_value_sum += tval;
                    }
                }
                if (attacked_targets >= 2) {
                    sc += 260000 + attacked_value_sum * 16;
                    is_tactical_drop = true;
                } else if (attacked_targets == 1) {
                    sc += 20000 + attacked_value_sum * 8;
                    is_tactical_drop = true;
                }
            }

            if (!is_tactical_drop && sc < 180000) {
                if (can_rotate(m.ptype)) {
                    int my_fq = eval_fq_run(i, true, frank);
                    if (my_fq < fq_best) {
                        sc -= 85000 + static_cast<int>(frank) * 6000;
                    } else if (is_far_drop(m, own_king, enemy_king)) {
                        sc -= 50000;
                    }
                } else if (is_far_drop(m, own_king, enemy_king)) {
                    sc -= 50000;
                }
            }
        } else {
            // Single, Team, or Rotate quiet move
            if (!quiets_only && ply < MAX_PLY) {
                if (m == killers_[ply][0]) sc = 800000;
                else if (m == killers_[ply][1]) sc = 750000;
                else if (!counter_move.is_none() && m == counter_move) sc = 700000;
            }
            int end_sq = (m.kind == MoveKind::Rotate) ? m.src : m.dst;
            uint8_t end_f = static_cast<uint8_t>((pos.top_piece[m.src].facing + m.rotate_after) & 3);
            if (end_sq >= 0 && end_sq < NUM_SQUARES) {
                sc += std::clamp(history_[stm][m.ptype][end_f][end_sq], -HIST_MAX, HIST_MAX);
                sc += read_5ply_ch(m.ptype, static_cast<int8_t>(end_f), end_sq);
            }

            // Facing equivalence grouping for Single/Team moves on same (kind, src, dst)
            if ((m.kind == MoveKind::Single || m.kind == MoveKind::Team) && can_rotate(m.ptype) && sc < 180000) {
                int my_fq = eval_fq_run(i, false, frank);
                if (my_fq == fq_best) {
                    sc += 8000;
                } else {
                    sc -= 38000 + static_cast<int>(frank) * 6000;
                }
            }

            if (ply <= 2) {
                if (m.src >= 0 && m.dst >= 0 && src_danger[m.src] > 0) {
                    if (pos.height[m.src] == 1 && pos.top_piece[m.src].type == BINH) {
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
            if (m.kind == MoveKind::Rotate && sc < 180000 && (root_ply_ + ply) <= 16) {
                if (!move_may_give_check(pos, m, enemy_king) &&
                    is_passive_rotate(pos, m, enemy, enemy_king)) {
                    sc -= 45000;
                }
            }
            if (koma_on && sc < 180000) {
                bool is_kil = (!quiets_only && ply < MAX_PLY && (m == killers_[ply][0] || m == killers_[ply][1]));
                if (koma_fast) {
                    if (!is_kil && is_bad_koma_quiet(pos, m, stm) &&
                        !move_may_give_check(pos, m, enemy_king) &&
                        !is_src_attacked(m.src)) {
                        sc -= 120000;
                    }
                } else {
                    if (!is_kil && !move_may_give_check(pos, m, enemy_king) &&
                        !is_src_attacked(m.src) &&
                        is_bad_koma_quiet(pos, m, stm)) {
                        sc -= 120000;
                    }
                }
            }
        }
        scores[i] = sc;
        if (out_fam_rank) out_fam_rank[i] = frank;
    }
}

int Searcher::qsearch(Position& pos, int alpha, int beta, int ply, int qply, bool* out_rep_tainted,
                      int8_t known_in_check, int known_rep) {
#ifdef HOMEI_PROFILE
    SearchProfile& prof = get_search_profile();
    const bool prof_root_q = (prof.enabled && qply == 0);
    std::chrono::steady_clock::time_point t_q0;
    if (prof.enabled) {
        ++prof.qsearch_calls;
        if (prof_root_q) t_q0 = std::chrono::steady_clock::now();
    }
    struct QProfGuard {
        bool on;
        std::chrono::steady_clock::time_point t0;
        ~QProfGuard() {
            if (on) {
                auto t1 = std::chrono::steady_clock::now();
                get_search_profile().qsearch_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
                );
            }
        }
    } qguard{prof_root_q, t_q0};
#endif

    pv_length_[ply] = ply;
    if (abort_ || (nodes_ >= limits_.max_nodes && check_stop())) return 0;

    // 1. Check 3-fold repetition first (Section 4.2)
    int rep = 1;
    bool node_rep_tainted = false;
    if (ply >= 1) {
        rep = (known_rep >= 0) ? known_rep : count_repetitions(pos.zobrist, ply);
        if (rep >= 2) node_rep_tainted = true;
        if (rep >= 3) {
            if (out_rep_tainted) *out_rep_tainted = true;
            return SCORE_MATE - ply;
        }
    }

    if (ply >= MAX_PLY - 1 || qply >= 10) {
        if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
        bool dummy_lazy = false;
        return eval_node(pos, ply, -SCORE_INFINITE, SCORE_INFINITE, false, dummy_lazy);
    }

    Side stm = static_cast<Side>(pos.side_to_move);
    Side enemy = other_side(stm);
    bool in_chk = (known_in_check >= 0) ? (known_in_check != 0) : is_in_check(pos, stm);
    const EngineFeatures& qfeats = g_sfeats;

    TTEntry qtte;
    Move qtt_move = Move::none();
    bool qtt_hit = false;
    if (!in_extract_qleaf_ && rep == 1) {
        qtt_hit = tt_ptr_->probe(pos.zobrist, qtte);
        if (qtt_hit) {
            qtt_move = qtte.best_move;
            if (!in_chk && qtte.depth >= 0) {
                int tt_score = score_from_tt(qtte.score, ply);
                if (qtte.bound == TT_EXACT) return tt_score;
                if (qtte.bound == TT_LOWER && tt_score >= beta) return tt_score;
                if (qtte.bound == TT_UPPER && tt_score <= alpha) return tt_score;
            }
        }
    }

    MoveList& legal = ply_stack_[ply].moves;
    int* scores = ply_stack_[ply].scores;

    if (in_chk) {
        // When in check, generate all legal evasions cleanly
        generate_evasions(pos, legal);
        if (legal.count == 0) {
            if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
            return -(SCORE_MATE - ply);
        }

        score_moves(pos, legal, scores, qtt_move, ply);

        int best_score = -SCORE_INFINITE;
        bool seen_non_rep3 = false;
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
            nnue_invalidate(ply + 1);
            ++nodes_;
#ifdef HOMEI_STATS
            ++stats_.qnodes;
#endif
            if ((nodes_ & 255ULL) == 0 && check_stop()) {
                undo_move(pos, m, u);
                return 0;
            }
            path_hashes_[ply + 1] = pos.zobrist;

            int score = 0;
            int child_rep_cnt = count_repetitions(pos.zobrist, ply + 1);
            if (child_rep_cnt >= 3) {
                node_rep_tainted = true;
                undo_move(pos, m, u);
                if (seen_non_rep3 || has_any_non_rep3_move(pos, ply)) {
                    seen_non_rep3 = true;
                    score = -(SCORE_MATE - (ply + 1)); // Điều 4
                } else {
                    score = 0; // Điều 5: Thiên Nhật Hòa
                }
            } else {
                seen_non_rep3 = true;
                bool child_rep = false;
                score = -qsearch(pos, -beta, -alpha, ply + 1, qply + 1, &child_rep, -1, child_rep_cnt);
                undo_move(pos, m, u);
                if (child_rep) node_rep_tainted = true;
                if (abort_) return 0;
            }

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
        if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
        return best_score;
    }

    int orig_alpha = alpha;
    int stand_pat = 0;
    int16_t raw_static_eval = EVAL_NONE;
    bool got_tt_eval = false;
    if (qfeats.tt_static_eval && qtt_hit && rep == 1 && qtte.static_eval != EVAL_NONE) {
        stand_pat = qtte.static_eval;
        raw_static_eval = qtte.static_eval;
        got_tt_eval = true;
#ifdef HOMEI_PROFILE
        if (prof.enabled) ++prof.eval_tt_hits;
#endif
    }
    if (!got_tt_eval) {
        bool is_lazy = false;
        stand_pat = eval_node(pos, ply, alpha, beta, qfeats.lazy_eval && ply > 2, is_lazy);
        if (!is_lazy) {
            raw_static_eval = static_cast<int16_t>(std::clamp(stand_pat, -30000, 30000));
            if (qfeats.tt_static_eval && rep == 1 && !in_extract_qleaf_) {
                tt_ptr_->store_static_eval(pos.zobrist, raw_static_eval);
            }
        }
    }
    // Zero-window non-mate stand-pat fail-high cuts off BEFORE mate1_leaf (exact same fail-high to parent!)
    if (stand_pat >= beta && (beta - alpha <= 1) && beta < SCORE_MATE - 100) {
        if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
        return stand_pat;
    }

    // Tier 3: mate1_leaf fast bitboard scan at qply == 0 (runs on PV, mate windows, and all stand_pat < beta nodes!)
    if (qfeats.mate1_leaf && qply == 0 && !in_chk) {
#ifdef HOMEI_PROFILE
        std::chrono::steady_clock::time_point tm0;
        if (prof.enabled) { ++prof.mate1_calls; tm0 = std::chrono::steady_clock::now(); }
#endif
        const int ek = pos.king_sq[enemy];
        if (ek < NUM_SQUARES) {
            bool can_check_ek = false;
            if (pos.reserve[stm] != 0) {
                can_check_ek = (((ortho_attacks(ek, pos.bb_occ) | diag_attacks(ek, pos.bb_occ)) & BB_DROP_ROW[stm]).any());
            } else {
                uint8_t has_pris = 0;
                for (int pt = 1; pt < 8; ++pt) has_pris |= pos.prisoners[stm][pt];
                const Bitboard own_bb = pos.bb_side[stm];
                can_check_ek = (has_pris != 0) ||
                               ((own_bb & (pos.bb_type[LONG] | pos.bb_type[PHUONG] | pos.bb_type[XA] | pos.bb_stack)).any()) ||
                               ((own_bb & BB_CHEB2[ek]).any()) ||
                               ((own_bb & pos.bb_type[KI] & BB_CHEB4[ek]).any());
            }
            if (can_check_ek) {
                int escapes = 0;
                Bitboard kd = BB_STEP_ATTACKS[VUONG][0][ek] & ~pos.bb_side[enemy];
                if (kd.popcount() > 2) {
                    while (kd.any()) {
                        int sq = kd.pop_lsb();
                        if (!is_attacked(pos, sq, stm)) {
                            if (++escapes > 2) break;
                        }
                    }
                }
                if (escapes <= 2) {
                    Move m1 = Move::none();
                    if (has_mate_in_1(pos, m1, path_hashes_, ply, &mate_rep_, 0)) {
#ifdef HOMEI_PROFILE
                        if (prof.enabled) prof.mate1_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tm0).count());
#endif
                        int score = SCORE_MATE - (ply + 1);
                        if (!node_rep_tainted && rep == 1) {
                            tt_ptr_->store(pos.zobrist, 1, ply, score, TT_EXACT, m1);
                        }
                        pv_table_[ply][ply] = m1;
                        pv_length_[ply] = ply + 1;
                        if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
                        return score;
                    }
                }
            }
        }
#ifdef HOMEI_PROFILE
        if (prof.enabled) prof.mate1_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tm0).count());
#endif
    }

    if (stand_pat >= beta) {
        if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
        return stand_pat;
    }
    if (stand_pat > alpha) {
        alpha = stand_pat;
    }

    MoveList& caps = ply_stack_[ply].caps;
    generate_capture_moves(pos, caps);

    int enemy_king = pos.king_sq[enemy];
    const SearchParams& sp = g_sp;
    // Stage captures with Delta Pruning + MVV-LVA + Capture History + TT move; defer SEE until picked!
    legal.clear();
    for (int i = 0; i < caps.count; ++i) {
        const Move m = caps.moves[i];
        bool is_tt_cap = (!qtt_move.is_none() && m == qtt_move);
        bool is_multi_stack_cap = (m.dst >= 0 && pos.height[m.dst] >= 2);
        int max_gain = cell_exchange_swing(pos, m.dst);
        if (!is_tt_cap && !(qply == 0 && is_multi_stack_cap) &&
            stand_pat + max_gain + sp.qs_delta_margin < alpha &&
            !move_may_give_check(pos, m, enemy_king)) {
            continue;
        }
        int v_dst = cell_material_value(pos, m.dst);
        int v_att = (m.kind == MoveKind::Team)
            ? cell_material_value(pos, m.src)
            : piece_material_value(pos.top_piece[m.src].type);
        uint8_t vic_pt = pos.top_piece[m.dst].type;
        int chist = (m.dst >= 0 && m.dst < NUM_SQUARES) ? cap_history_[stm][m.ptype][vic_pt][m.dst] : 0;
        scores[legal.count] = is_tt_cap ? 2000000 : (1000000 + (10 * v_dst - v_att) * 4 + chist);
        legal.add(m);
    }

    Bitboard q_pinners;
    Bitboard q_pin_blockers = (legal.count > 0) ? slider_blockers(pos, stm, q_pinners) : Bitboard{};

    Move best_qmove = Move::none();
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
        bool is_multi_stack_cap = (m.dst >= 0 && pos.height[m.dst] >= 2);
        bool is_tt_cap = (!qtt_move.is_none() && m == qtt_move);
        if (!is_tt_cap && !(qply == 0 && is_multi_stack_cap)) {
            if (!see_ge(pos, m, 0)) {
                continue;
            }
        }
        if (!check_move_leaves_king_safe_pinned(pos, m, q_pin_blockers)) {
            continue;
        }

        UndoState u;
        apply_move(pos, m, u);
        nnue_invalidate(ply + 1);
        tt_ptr_->prefetch(pos.zobrist);
        ++nodes_;
#ifdef HOMEI_STATS
        ++stats_.qnodes;
#endif
        if ((nodes_ & 511ULL) == 0 && check_stop()) {
            undo_move(pos, m, u);
            return 0;
        }
        path_hashes_[ply + 1] = pos.zobrist;

        int score = 0;
        int child_rep_cnt = count_repetitions(pos.zobrist, ply + 1);
        if (child_rep_cnt >= 3) {
            node_rep_tainted = true;
            undo_move(pos, m, u);
            score = has_any_non_rep3_move(pos, ply) ? -(SCORE_MATE - (ply + 1)) : 0;
        } else {
            bool child_rep = false;
            score = -qsearch(pos, -beta, -alpha, ply + 1, qply + 1, &child_rep, -1, child_rep_cnt);
            undo_move(pos, m, u);
            if (child_rep) node_rep_tainted = true;
            if (abort_) return 0;
        }

        if (score > stand_pat) {
            stand_pat = score;
            best_qmove = m;
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

    if (!abort_ && !in_extract_qleaf_ && rep == 1 && !node_rep_tainted) {
        uint8_t qbound = (stand_pat <= orig_alpha) ? TT_UPPER : ((stand_pat >= beta) ? TT_LOWER : TT_EXACT);
        tt_ptr_->store(pos.zobrist, 0, ply, stand_pat, qbound, best_qmove, raw_static_eval);
    }

    if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
    return stand_pat;
}

int Searcher::pvs(
    Position& pos, int depth, int alpha, int beta, int ply, bool is_pv, int ext_budget,
    bool allow_nmp, Move prev_move, Move prev2_move, int8_t prev1_f, int8_t prev2_f, Move excluded_move,
    bool* out_rep_tainted, int8_t known_in_check, int known_rep,
    bool cut_node, int double_ext
) {
    pv_length_[ply] = ply;
    eval_stack_[ply] = EVAL_NONE;
    if (abort_ || (nodes_ >= limits_.max_nodes && check_stop())) return 0;

    // 1. Check 3-fold repetition BEFORE checkmate/pat (Section 4.2)
    int rep_count = 1;
    bool node_rep_tainted = false;
    if (ply >= 1) {
        rep_count = (known_rep >= 0) ? known_rep : count_repetitions(pos.zobrist, ply);
        if (rep_count >= 2) node_rep_tainted = true;
        if (rep_count >= 3) {
            if (out_rep_tainted) *out_rep_tainted = true;
            return SCORE_MATE - ply;
        }
        // YaneuraOu Superior/Inferior Hand Detection (same board_key within 16 ply, strictly better/worse prisoner hand)
        if (ply >= 2 && excluded_move.is_none()) {
            int sh = check_superior_inferior_hand(pos, ply);
            if (sh != 0) {
                node_rep_tainted = true;
                if (out_rep_tainted) *out_rep_tainted = true;
                return (sh > 0) ? (SCORE_SUPERIOR - ply) : -(SCORE_SUPERIOR - ply);
            }
        }
    }

    if (ply >= MAX_PLY - 1) {
        if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
        bool dummy_lazy = false;
        return eval_node(pos, ply, -SCORE_INFINITE, SCORE_INFINITE, false, dummy_lazy);
    }

    // Mate-Distance Pruning (MDP)
    int mating_value = SCORE_MATE - ply;
    if (alpha < -mating_value) alpha = -mating_value;
    if (beta > mating_value - 1) beta = mating_value - 1;
    if (alpha >= beta) {
        if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
        return alpha;
    }

    if (depth <= 0) {
        bool q_rep = false;
        int qs = qsearch(pos, alpha, beta, ply, 0, &q_rep, known_in_check, rep_count);
        if (out_rep_tainted && (node_rep_tainted || q_rep)) *out_rep_tainted = true;
        return qs;
    }

    // 2. Transposition Table Probe (disabled for cutoff when rep_count >= 2 or singular verification)
    TTEntry tte;
    Move tt_move = Move::none();
#ifdef HOMEI_STATS
    ++stats_.tt_probe;
#endif
    bool tt_hit = excluded_move.is_none() ? tt_ptr_->probe(pos.zobrist, tte) : false;
    int tt_score = 0;
    if (tt_hit) {
#ifdef HOMEI_STATS
        ++stats_.tt_hit;
#endif
        tt_move = tte.best_move;
        tt_score = score_from_tt(tte.score, ply);
        if (!is_pv && rep_count == 1 && tte.depth >= depth && tte.depth >= 0) {
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
    bool in_chk = (known_in_check >= 0) ? (known_in_check != 0) : is_in_check(pos, stm);
    const EngineFeatures& pfeats = g_sfeats;
    int16_t raw_static_eval = (tt_hit && pfeats.tt_static_eval) ? tte.static_eval : EVAL_NONE;

    // Internal Iterative Reductions (IIR): reduce depth when no TT move exists
    if (depth >= 4 && tt_move.is_none() && !in_chk && rep_count == 1 && excluded_move.is_none()) {
        --depth;
    }

    // Tier 2: mate_pvlight on PV nodes (main search thread only; keeps workers free of df-pn storage/contention)
    if (!is_worker_ && pfeats.mate_pvlight && is_pv && !in_chk && excluded_move.is_none() && ply >= 1 && ply <= 2 && depth >= 4 &&
        std::abs(alpha) < SCORE_MATE - 100 && std::abs(beta) < SCORE_MATE - 100) {
        MateResult res = solve_mate_bounded(pos, 5, 150, stop_flag_, &mate_rep_, path_hashes_, ply);
        nodes_ += res.nodes;
        if (res.found && !res.pv.empty()) {
            int mate_score = SCORE_MATE - (ply + res.mate_ply);
            if (res.rep_dependent) {
                node_rep_tainted = true;
            } else if (!node_rep_tainted && rep_count == 1) {
                tt_ptr_->store(pos.zobrist, depth, ply, mate_score, TT_EXACT, res.pv[0], raw_static_eval);
            }
            for (size_t mi = 0; mi < res.pv.size() && ply + mi < MAX_PLY; ++mi) {
                pv_table_[ply][ply + mi] = res.pv[mi];
            }
            pv_length_[ply] = ply + static_cast<int>(res.pv.size());
            if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
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
    bool improving = false;
    if (!in_chk && rep_count == 1) {
        int corr = correction_delta(pos, ply);
        int raw_eval = 0;
        if (raw_static_eval != EVAL_NONE) {
            raw_eval = raw_static_eval;
#ifdef HOMEI_PROFILE
            if (get_search_profile().enabled) ++get_search_profile().eval_tt_hits;
#endif
        } else {
            bool is_lazy = false;
            raw_eval = eval_node(pos, ply, alpha - corr, beta - corr, !is_pv && pfeats.lazy_eval && ply > 2, is_lazy);
            if (!is_lazy) {
                raw_static_eval = static_cast<int16_t>(std::clamp(raw_eval, -30000, 30000));
                if (pfeats.tt_static_eval && excluded_move.is_none()) {
                    tt_ptr_->store_static_eval(pos.zobrist, raw_static_eval);
                }
            }
        }
        static_eval = std::clamp(raw_eval + corr, -SCORE_SUPERIOR + 200, SCORE_SUPERIOR - 200);
        eval_stack_[ply] = static_cast<int16_t>(std::clamp(static_eval, -30000, 30000));
        if (ply >= 2 && eval_stack_[ply - 2] != EVAL_NONE) {
            improving = (static_eval > static_cast<int>(eval_stack_[ply - 2]));
        } else if (ply >= 4 && eval_stack_[ply - 4] != EVAL_NONE) {
            improving = (static_eval > static_cast<int>(eval_stack_[ply - 4]));
        }

        // Hindsight Reduction Correction (safety re-extension only: undo over-reduction if eval surprised us)
        if (ply >= 1 && excluded_move.is_none() && eval_stack_[ply - 1] != EVAL_NONE) {
            int prev_r = static_cast<int>(reduction_stack_[ply - 1]);
            int eval_sum = static_eval + static_cast<int>(eval_stack_[ply - 1]);
            if (prev_r >= 3 && eval_sum < -30 && depth < MAX_PLY - 4) {
                ++depth;
            }
        }
    }

    const SearchParams& sp = g_sp;

    if (!is_pv && !in_chk && rep_count == 1 && excluded_move.is_none()) {
        // Reverse Futility Pruning (RFP / Static Null Move) with improving margin
        if (ENABLE_RFP && depth <= 5 && std::abs(beta) < SCORE_SUPERIOR - 200) {
            int margin = (improving ? sp.rfp_imp : sp.rfp_noimp) * depth;
            if (static_eval - margin >= beta) {
                return beta;
            }
        }

        // Razoring
        if (ENABLE_RAZORING && depth <= 2 && std::abs(alpha) < SCORE_SUPERIOR - 200 && std::abs(beta) < SCORE_SUPERIOR - 200) {
            int razor_margin = improving ? sp.razor_imp : sp.razor_noimp;
            if (static_eval + razor_margin < alpha) {
                bool r_rep = false;
                int r_score = qsearch(pos, alpha, beta, ply, 0, &r_rep, 0, rep_count);
                if (abort_) return 0;
                if (r_score < alpha) {
                    if (out_rep_tainted && (node_rep_tainted || r_rep)) *out_rep_tainted = true;
                    return r_score;
                }
            }
        }

        // Null-Move Pruning (NMP)
        if (ENABLE_NMP && allow_nmp && depth >= 3 && static_eval >= beta &&
            std::abs(beta) < SCORE_SUPERIOR - 200 && !pos.has_reserve(stm)) {

            // Zugzwang check via bitboard: must have non-King mobile material
            bool has_non_king_mat = false;
            for (int pt = 1; pt < 8; ++pt) {
                if (pos.prisoners[stm][pt] > 0) {
                    has_non_king_mat = true;
                    break;
                }
            }
            if (!has_non_king_mat) {
                if (pos.king_sq[stm] < NUM_SQUARES) {
                    has_non_king_mat = (pos.bb_side[stm] & ~Bitboard::from_sq(pos.king_sq[stm])).any();
                } else {
                    has_non_king_mat = pos.bb_side[stm].any();
                }
            }

            if (has_non_king_mat) {
                int R = 3 + depth / 4 + std::min(3, (static_eval - beta) / sp.nmp_eval_div);
                int null_depth = std::max(0, depth - 1 - R);

                uint64_t prev_bk = pos.board_key;
                uint64_t prev_zob = pos.zobrist;
                pos.side_to_move = other_side(stm);
                pos.board_key ^= zobrist_side;
                pos.zobrist ^= zobrist_side;
                nnue_invalidate(ply + 1);
                tt_ptr_->prefetch(pos.zobrist);
                // Sentinel 0ULL prevents phantom null-move hash from polluting repetition / superior-hand history
                path_hashes_[ply + 1] = 0ULL;
                path_board_keys_[ply + 1] = 0ULL;
                move_ptsq_stack_[ply] = -1;
                move_pfsq_stack_[ply] = -1;
                move_stack_[ply] = Move::none();
                reduction_stack_[ply] = 0;

                bool null_rep = false;
                int null_score = -pvs(pos, null_depth, -beta, -beta + 1, ply + 1, false, 0, false,
                                      Move::none(), Move::none(), 0, 0, Move::none(), &null_rep, -1, -1, !cut_node, double_ext);

                pos.side_to_move = stm;
                pos.board_key = prev_bk;
                pos.zobrist = prev_zob;
                if (abort_) return 0;

                if (!null_rep && null_score >= beta) {
                    if (depth < 8) {
#ifdef HOMEI_STATS
                        ++stats_.null_move_cutoffs;
#endif
                        return beta;
                    }
                    // Verification search at high depth (d >= 8)
                    bool verif_rep = false;
                    int verif_score = pvs(pos, null_depth, beta - 1, beta, ply, false, 0, false,
                                          Move::none(), Move::none(), 0, 0, Move::none(), &verif_rep, 0, rep_count, cut_node, double_ext);
                    if (abort_) return 0;
                    if (!verif_rep && verif_score >= beta) {
#ifdef HOMEI_STATS
                        ++stats_.null_move_cutoffs;
#endif
                        return beta;
                    }
                } else if (!null_rep && pfeats.mate_threat && null_score <= -SCORE_MATE + MAX_PLY) {
                    mate_threat = true;
                }
            }
        }

        // YaneuraOu ProbCut (depth >= 5): Small TT ProbCut (depth >= 7 only) + Sorted Captures + qsearch gate
        if (depth >= 5 && std::abs(beta) < SCORE_SUPERIOR - 200) {
            int prob_beta = beta + sp.probcut_margin;
            if (depth >= 7 && tt_hit && tte.depth >= depth - 3 && tte.bound == TT_LOWER &&
                tt_score >= prob_beta + 80 && std::abs(tt_score) < SCORE_SUPERIOR - 200) {
                return prob_beta;
            }
            int see_thresh = std::max(0, prob_beta - static_eval);
            MoveList& cap_moves = ply_stack_[ply].caps;
            MoveList& pc_cands = ply_stack_[ply].moves;
            int* pc_scores = ply_stack_[ply].scores;
            pc_cands.clear();
            Bitboard pc_pinners;
            Bitboard pc_pin_blockers = slider_blockers(pos, stm, pc_pinners);
            if (pfeats.fast_probcut) {
                generate_capture_moves(pos, cap_moves);
            } else {
                generate_legal_moves(pos, cap_moves);
            }
            MoveKind last_k = MoveKind::Rotate;
            int8_t last_s = -2, last_d = -2;
            bool last_ok = false;
            int last_mvv = 0;
            for (int i = 0; i < cap_moves.count; ++i) {
                const Move cm = cap_moves.moves[i];
                if (!pfeats.fast_probcut && !is_capture_move(pos, cm)) continue;
                bool ok;
                if (cm.kind == last_k && cm.src == last_s && cm.dst == last_d) {
                    ok = last_ok;
                } else {
                    ok = see_ge(pos, cm, see_thresh) &&
                         (!pfeats.fast_probcut || check_move_leaves_king_safe_pinned(pos, cm, pc_pin_blockers));
                    last_k = cm.kind; last_s = cm.src; last_d = cm.dst; last_ok = ok;
                    if (ok) {
                        int v_dst = cell_material_value(pos, cm.dst);
                        int v_att = (cm.kind == MoveKind::Team)
                            ? cell_material_value(pos, cm.src)
                            : piece_material_value(pos.top_piece[cm.src].type);
                        last_mvv = 10 * v_dst - v_att;
                    }
                }
                if (ok && pc_cands.count < MAX_MOVES) {
                    pc_scores[pc_cands.count] = last_mvv;
                    pc_cands.add(cm);
                }
            }
            for (int i = 0; i < pc_cands.count; ++i) {
                int best_idx = i;
                for (int j = i + 1; j < pc_cands.count; ++j) {
                    if (pc_scores[j] > pc_scores[best_idx]) best_idx = j;
                }
                if (best_idx != i) {
                    std::swap(pc_cands.moves[i], pc_cands.moves[best_idx]);
                    std::swap(pc_scores[i], pc_scores[best_idx]);
                }
                const Move cm = pc_cands.moves[i];
                int8_t cmf = move_end_facing(pos, cm);
                move_stack_[ply] = cm;
                move_f_stack_[ply] = cmf;
                move_ptsq_stack_[ply] = static_cast<int16_t>(ptsq_idx(cm.ptype, cm.dst));
                move_pfsq_stack_[ply] = static_cast<int16_t>(pfsq_idx(cm.ptype, cmf, cm.dst));
                reduction_stack_[ply] = 0;

                UndoState u_prob;
                apply_move(pos, cm, u_prob);
                nnue_invalidate(ply + 1);
                tt_ptr_->prefetch(pos.zobrist);
                ++nodes_;
                record_path_node(pos, ply + 1);
                int pc_rep_cnt = count_repetitions(pos.zobrist, ply + 1);
                if (pc_rep_cnt >= 3) {
                    undo_move(pos, cm, u_prob);
                    continue;
                }
                bool prob_rep = false;
                int prob_score = -qsearch(pos, -prob_beta, -prob_beta + 1, ply + 1, 0, &prob_rep, -1, pc_rep_cnt);
                if (!abort_ && !prob_rep && prob_score >= prob_beta) {
                    prob_score = -pvs(pos, depth - 4, -prob_beta, -prob_beta + 1, ply + 1, false, 0, false,
                                      cm, prev_move, cmf, prev1_f, Move::none(), &prob_rep, -1, pc_rep_cnt, !cut_node, double_ext);
                }
                undo_move(pos, cm, u_prob);
                if (abort_) return 0;
                if (!prob_rep && prob_score >= prob_beta) {
                    tt_ptr_->store(pos.zobrist, depth - 3, ply, prob_score, TT_LOWER, cm, raw_static_eval);
                    return prob_beta;
                }
            }
        }
    }

    if (mate_threat && ext_budget > 0) {
        ++depth;
        --ext_budget;
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

    MoveList& legal = ply_stack_[ply].moves;
    MoveList& caps_buf = ply_stack_[ply].caps;
    MoveList& bad_caps_buf = ply_stack_[ply].bad_caps;
    int* scores = ply_stack_[ply].scores;
    int* bad_cap_scores = ply_stack_[ply].bad_cap_scores;
    uint8_t* fam_ranks = ply_stack_[ply].fam_rank;

    const bool use_staged = (pfeats.staged_movegen && !in_chk);
    if (!use_staged) {
        if (in_chk) {
            generate_evasions(pos, legal);
        } else {
            generate_legal_moves(pos, legal);
        }
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
        score_moves(pos, legal, scores, tt_move, ply, counter_move, prev_move, prev2_move, prev1_f, prev2_f, false, fam_ranks);
    }

    int orig_alpha = alpha;
    int best_score = -SCORE_INFINITE;
    Move best_move = Move::none();

    int own_king = pos.king_sq[stm];
    int enemy_king = pos.king_sq[enemy];
    const bool tt_capture = (!tt_move.is_none() && is_capture_move(pos, tt_move));

    int futility_margin = (improving ? sp.futility_imp : sp.futility_noimp) * depth;
    bool futility_pruning = !mate_threat && (ENABLE_FUTILITY && !is_pv && !in_chk && depth <= 3 && rep_count == 1 &&
                             std::abs(alpha) < SCORE_SUPERIOR - 200 &&
                             std::abs(beta) < SCORE_SUPERIOR - 200 &&
                             (static_eval + futility_margin < alpha));

    bool lmp_active = !mate_threat && (ENABLE_LMP && !is_pv && !in_chk && depth <= 4 && rep_count == 1 &&
                       std::abs(alpha) < SCORE_SUPERIOR - 200 &&
                       std::abs(beta) < SCORE_SUPERIOR - 200);

    int moves_searched = 0;
    int quiet_searched = 0;
    Move quiet_tried[64]{};
    int8_t quiet_tried_f[64]{};
    int n_quiet_tried = 0;
    Move cap_tried[32]{};
    uint8_t cap_tried_vic[32]{};
    int n_cap_tried = 0;

    enum StagedPhase : int {
        SPHASE_TT = 0,
        SPHASE_GEN_CAPS,
        SPHASE_GOOD_CAPS,
        SPHASE_KILLERS,
        SPHASE_GEN_QUIETS,
        SPHASE_QUIETS,
        SPHASE_BAD_CAPS,
        SPHASE_DONE
    };
    int sphase = SPHASE_TT;
    int cur_idx = 0;
    int pos_end = 0;
    int sorted_until = 0;
    int special_idx = 0;
    bool skip_quiets = false;
    Move special_cands[3] = {
        (ply < MAX_PLY) ? killers_[ply][0] : Move::none(),
        (ply < MAX_PLY) ? killers_[ply][1] : Move::none(),
        counter_move
    };
    int special_sc[3] = {800000, 750000, 700000};
    Move yielded_specials[3]{};
    int num_yielded_specials = 0;
    bool tt_yielded = false;

    Bitboard node_pinners;
    Bitboard node_pin_blockers = use_staged ? slider_blockers(pos, stm, node_pinners) : Bitboard{};

    auto next_move_fn = [&](Move& out_m, int& out_sc, uint8_t& out_frank) -> bool {
        if (!use_staged) {
            if (cur_idx >= legal.count) return false;
            int best_idx = cur_idx;
            for (int j = cur_idx + 1; j < legal.count; ++j) {
                if (scores[j] > scores[best_idx]) best_idx = j;
            }
            if (best_idx != cur_idx) {
                std::swap(legal.moves[cur_idx], legal.moves[best_idx]);
                std::swap(scores[cur_idx], scores[best_idx]);
                std::swap(fam_ranks[cur_idx], fam_ranks[best_idx]);
            }
            out_m = legal.moves[cur_idx];
            out_sc = scores[cur_idx];
            out_frank = fam_ranks[cur_idx];
            ++cur_idx;
            return true;
        }

        while (sphase != SPHASE_DONE) {
            if (sphase == SPHASE_TT) {
                sphase = SPHASE_GEN_CAPS;
                if (!tt_move.is_none() && is_pseudo_legal_move(pos, tt_move) &&
                    check_move_leaves_king_safe_pinned(pos, tt_move, node_pin_blockers)) {
                    tt_yielded = true;
                    out_m = tt_move;
                    out_sc = 2000000;
                    out_frank = 0;
                    return true;
                }
            } else if (sphase == SPHASE_GEN_CAPS) {
                // Decouple legality & exact Stack-SEE per (kind, src, dst) for CLP stack-capture ordering accuracy
                generate_capture_moves(pos, caps_buf);
                legal.clear();
                bad_caps_buf.clear();
                int ekr_c = (enemy_king < NUM_SQUARES) ? sq_row(enemy_king) : -1;
                int ekc_c = (enemy_king < NUM_SQUARES) ? sq_col(enemy_king) : -1;

                MoveKind last_k = MoveKind::Rotate;
                int8_t last_s = -2, last_d = -2;
                bool last_leg = false;
                int last_see = 0, last_vdst = 0, last_vatt = 0;

                for (int ci = 0; ci < caps_buf.count; ++ci) {
                    const Move cm = caps_buf.moves[ci];
                    if (tt_yielded && cm == tt_move) continue;
                    bool same_base = (cm.kind == last_k && cm.src == last_s && cm.dst == last_d);
                    if (!same_base) {
                        last_k = cm.kind;
                        last_s = cm.src;
                        last_d = cm.dst;
                        last_leg = check_move_leaves_king_safe_pinned(pos, cm, node_pin_blockers);
                        if (last_leg) {
                            last_vdst = cell_material_value(pos, cm.dst);
                            last_vatt = (cm.kind == MoveKind::Team)
                                ? cell_material_value(pos, cm.src)
                                : piece_material_value(pos.top_piece[cm.src].type);
                            last_see = stack_see_estimate(pos, cm);
                        }
                    }
                    if (!last_leg) continue;

                    uint8_t vic_pt = pos.top_piece[cm.dst].type;
                    int chist = (cm.dst >= 0 && cm.dst < NUM_SQUARES) ? cap_history_[stm][cm.ptype][vic_pt][cm.dst] : 0;
                    int follow_bonus = 0;
                    if (ply <= 2 && can_rotate(cm.ptype)) {
                        uint8_t eff_f = static_cast<uint8_t>((pos.top_piece[cm.src].facing + cm.rotate_after) & 3);
                        const PieceMoveTable& tbl = get_piece_moves(cm.ptype, eff_f);
                        int rd = sq_row(cm.dst);
                        int cd = sq_col(cm.dst);
                        for (int vi = 0; vi < tbl.count; ++vi) {
                            const MoveVec& mv = tbl.vecs[vi];
                            if (!mv.sliding) {
                                int tr = rd + mv.dr;
                                int tc = cd + mv.dc;
                                if (in_bounds(tr, tc)) {
                                    int tsq = make_sq(tr, tc);
                                    if (tsq != cm.src && pos.height[tsq] > 0 && pos.cell_owner(tsq) == enemy) {
                                        follow_bonus += 16 + cell_material_value(pos, tsq) / 32;
                                    }
                                }
                            }
                        }
                        if (ekr_c >= 0 && (std::abs(rd - ekr_c) + std::abs(cd - ekc_c)) <= 3) {
                            bool points_at_ek =
                                (eff_f == 0 && ekr_c < rd) ||
                                (eff_f == 2 && ekr_c > rd) ||
                                (eff_f == 1 && ekc_c > cd) ||
                                (eff_f == 3 && ekc_c < cd);
                            if (points_at_ek) follow_bonus += 12;
                        }
                    }
                    if (last_see >= 0) {
                        scores[legal.count] = 1000000 + last_see * 64 + (10 * last_vdst - last_vatt) + chist / 4 + follow_bonus;
                        legal.add(cm);
                    } else if (bad_caps_buf.count < MAX_MOVES) {
                        bad_cap_scores[bad_caps_buf.count] = -100000 + last_see * 64 + (10 * last_vdst - last_vatt) + chist / 4 + follow_bonus;
                        bad_caps_buf.add(cm);
                    }
                }
                cur_idx = 0;
                sphase = SPHASE_GOOD_CAPS;
            } else if (sphase == SPHASE_GOOD_CAPS) {
                if (cur_idx < legal.count) {
                    int best_idx = cur_idx;
                    for (int j = cur_idx + 1; j < legal.count; ++j) {
                        if (scores[j] > scores[best_idx]) best_idx = j;
                    }
                    if (best_idx != cur_idx) {
                        std::swap(legal.moves[cur_idx], legal.moves[best_idx]);
                        std::swap(scores[cur_idx], scores[best_idx]);
                    }
                    out_m = legal.moves[cur_idx];
                    out_sc = scores[cur_idx];
                    out_frank = 0;
                    ++cur_idx;
                    return true;
                }
                special_idx = 0;
                sphase = SPHASE_KILLERS;
            } else if (sphase == SPHASE_KILLERS) {
                bool found_special = false;
                while (special_idx < 3) {
                    Move km = special_cands[special_idx];
                    int ksc = special_sc[special_idx];
                    ++special_idx;
                    if (km.is_none()) continue;
                    if (tt_yielded && km == tt_move) continue;
                    bool dup = false;
                    for (int y = 0; y < num_yielded_specials; ++y) {
                        if (yielded_specials[y] == km) { dup = true; break; }
                    }
                    if (dup) continue;
                    if (km.kind == MoveKind::Drop && !pfeats.drop_history && km != counter_move) continue;
                    if (!is_pseudo_legal_move(pos, km)) continue;
                    if (is_capture_move(pos, km)) continue;
                    if (!check_move_leaves_king_safe_pinned(pos, km, node_pin_blockers)) continue;
                    yielded_specials[num_yielded_specials++] = km;
                    out_m = km;
                    out_sc = ksc;
                    out_frank = 0;
                    found_special = true;
                    break;
                }
                if (found_special) return true;
                sphase = SPHASE_GEN_QUIETS;
            } else if (sphase == SPHASE_GEN_QUIETS) {
                QuietGenFilter qfilter;
                if (!is_pv && !in_chk && !pos.has_reserve(stm)) {
                    if (futility_pruning && depth <= 2 && moves_searched > 0) {
                        qfilter.skip_far_noncheck_drops = true;
                    }
                    if (depth <= 3) {
                        qfilter.max_plain_drop_facings = 2;
                    }
                }
                generate_quiet_and_drop_moves(pos, caps_buf, qfilter);
                legal.clear();
                MoveKind last_qk = MoveKind::Rotate;
                int8_t last_qs = -2, last_qd = -2;
                bool last_qok = false;
                for (int qi = 0; qi < caps_buf.count; ++qi) {
                    const Move qm = caps_buf.moves[qi];
                    if (tt_yielded && qm == tt_move) continue;
                    bool dup = false;
                    for (int y = 0; y < num_yielded_specials; ++y) {
                        if (yielded_specials[y] == qm) { dup = true; break; }
                    }
                    if (dup) continue;
                    bool ok;
                    if (qm.kind == MoveKind::Drop || qm.kind == MoveKind::Rotate) {
                        ok = true; // !in_chk guaranteed in staged mode
                    } else if (qm.kind == last_qk && qm.src == last_qs && qm.dst == last_qd) {
                        ok = last_qok;
                    } else {
                        ok = check_move_leaves_king_safe_pinned(pos, qm, node_pin_blockers);
                        last_qk = qm.kind; last_qs = qm.src; last_qd = qm.dst; last_qok = ok;
                    }
                    if (!ok) continue;
                    legal.add(qm);
                }
                score_moves(pos, legal, scores, Move::none(), ply, counter_move, prev_move, prev2_move, prev1_f, prev2_f, true, fam_ranks);
                // Partition positive-scored quiets ahead of negative-scored secondary/far quiets (lockstep fam_ranks!)
                if (legal.count > 48) {
                    int left = 0;
                    int right = legal.count - 1;
                    while (left <= right) {
                        while (left <= right && scores[left] >= 0) ++left;
                        while (left <= right && scores[right] < 0) --right;
                        if (left < right) {
                            std::swap(legal.moves[left], legal.moves[right]);
                            std::swap(scores[left], scores[right]);
                            std::swap(fam_ranks[left], fam_ranks[right]);
                            ++left;
                            --right;
                        }
                    }
                    pos_end = left;
                } else {
                    pos_end = legal.count;
                }
                cur_idx = 0;
                sorted_until = 0;
                sphase = SPHASE_QUIETS;
            } else if (sphase == SPHASE_QUIETS) {
                if (skip_quiets || cur_idx >= legal.count) {
                    cur_idx = 0;
                    sphase = SPHASE_BAD_CAPS;
                    continue;
                }
                int seg_end = (cur_idx < pos_end) ? pos_end : legal.count;
                if (cur_idx == pos_end) {
                    sorted_until = pos_end;
                }
                pick_next_move(legal.moves, scores, cur_idx, seg_end, sorted_until, fam_ranks);
                out_m = legal.moves[cur_idx];
                out_sc = scores[cur_idx];
                out_frank = fam_ranks[cur_idx];
                ++cur_idx;
                return true;
            } else if (sphase == SPHASE_BAD_CAPS) {
                if (cur_idx < bad_caps_buf.count) {
                    int best_idx = cur_idx;
                    for (int j = cur_idx + 1; j < bad_caps_buf.count; ++j) {
                        if (bad_cap_scores[j] > bad_cap_scores[best_idx]) best_idx = j;
                    }
                    if (best_idx != cur_idx) {
                        std::swap(bad_caps_buf.moves[cur_idx], bad_caps_buf.moves[best_idx]);
                        std::swap(bad_cap_scores[cur_idx], bad_cap_scores[best_idx]);
                    }
                    out_m = bad_caps_buf.moves[cur_idx];
                    out_sc = bad_cap_scores[cur_idx];
                    out_frank = 0;
                    ++cur_idx;
                    return true;
                }
                sphase = SPHASE_DONE;
            }
        }
        return false;
    };

    Move m = Move::none();
    int move_order_score = 0;
    uint8_t curr_fam_rank = 0;
    bool seen_non_rep3 = false;
    // Lazily cached is_attacked(pos, m.src, enemy) across all moves from the same piece!
    Bitboard pvs_src_att_known{};
    Bitboard pvs_src_att_val{};
    auto get_src_attacked = [&](int ssq) -> bool {
        if (ssq < 0) return false;
        if (!pvs_src_att_known.test_bit(ssq)) {
            pvs_src_att_known.set_bit(ssq);
            if (is_attacked(pos, ssq, enemy)) pvs_src_att_val.set_bit(ssq);
        }
        return pvs_src_att_val.test_bit(ssq);
    };

    while (next_move_fn(m, move_order_score, curr_fam_rank)) {
        if (!excluded_move.is_none() && m == excluded_move) continue;
        if (best_move.is_none()) {
            best_move = m;
        }
        bool is_cap = is_capture_move(pos, m);
        uint8_t cap_vic_pt = is_cap ? pos.top_piece[m.dst].type : 0;
        bool is_tt = (!tt_move.is_none() && m == tt_move);
        bool is_killer = (ply < MAX_PLY && (m == killers_[ply][0] || m == killers_[ply][1]));
        bool is_tactical_score = (move_order_score >= 180000);
        int8_t curr_end_f = move_end_facing(pos, m);
        int end_sq = (m.kind == MoveKind::Rotate) ? m.src : m.dst;
        int8_t may_give_chk_cache = -1;
        auto get_may_give_chk = [&]() -> bool {
            if (may_give_chk_cache < 0) {
                may_give_chk_cache = move_may_give_check(pos, m, enemy_king) ? 1 : 0;
            }
            return may_give_chk_cache != 0;
        };

        // Precompute move history once for quiet leaf pruning + 1024-scale LMR
        int move_total_hist = 0;
        bool hist_computed = false;
        auto get_move_total_hist = [&]() -> int {
            if (!hist_computed) {
                int main_h = 0;
                if (end_sq >= 0 && end_sq < NUM_SQUARES) {
                    main_h = (m.kind == MoveKind::Drop && pfeats.drop_history)
                        ? drop_history_[stm][m.ptype][curr_end_f][end_sq]
                        : history_[stm][m.ptype][curr_end_f][end_sq];
                }
                move_total_hist = main_h + get_5ply_cont_hist(ply, m.ptype, curr_end_f, end_sq) * 2;
                hist_computed = true;
            }
            return move_total_hist;
        };

        // Quiet Move Pruning (Futility, YaneuraOu LMP + Move-Family Quotas, History Leaf Pruning, Quiet SEE Pruning)
        if (moves_searched > 0 && !is_cap && !is_pv && !in_chk && rep_count == 1 && !mate_threat &&
            (futility_pruning || lmp_active || depth <= 4)) {
            bool is_exempt = is_tt || is_killer || is_tactical_score || get_may_give_chk();
            if (!is_exempt) {
                if (m.kind == MoveKind::Drop) {
                    if (drop_threatens_enemy(pos, m, enemy, enemy_king)) {
                        is_exempt = true;
                    } else if (own_king >= 0 && own_king < NUM_SQUARES) {
                        int dk = std::max(std::abs(sq_row(m.dst) - sq_row(own_king)), std::abs(sq_col(m.dst) - sq_col(own_king)));
                        if (dk <= 2) is_exempt = true;
                    }
                } else if (get_src_attacked(m.src)) {
                    is_exempt = true;
                }
            }

            if (!is_exempt) {
                // 1. Futility Pruning
                if (futility_pruning) {
                    continue;
                }

                // 2. CLP-Calibrated LMP + Move-Family Quotas (preserve is_exempt checks/king-drops/escapes at all plies!)
                if (lmp_active) {
                    int lmp_limit = 3 + (improving ? 2 : 1) * depth * depth;
                    if (quiet_searched >= lmp_limit) {
                        continue;
                    }
                    bool passive_rot = is_passive_rotate(pos, m, enemy, enemy_king);
                    bool far_drp = is_far_drop(m, own_king, enemy_king);
                    if (curr_fam_rank >= 2 || passive_rot || far_drp) {
                        int low_limit = 2 + (depth * depth) / (improving ? 1 : 2);
                        if (quiet_searched >= low_limit) {
                            continue;
                        }
                    } else if (curr_fam_rank == 1) {
                        int sec_limit = 3 + (depth * depth) / (improving ? 1 : 2);
                        if (quiet_searched >= sec_limit) {
                            continue;
                        }
                    }
                }

                // 3. History Leaf Pruning (only clearly awful negative-scored quiets at shallow depth)
                if (depth <= 3 && moves_searched >= 4 && move_order_score < 0 && get_move_total_hist() < -12000 * depth) {
                    continue;
                }
            }
        }

        // Compute pre-move state BEFORE apply_move mutates pos
        bool pre_bad_koma_quiet = false;
        bool pre_passive_rotate = false;
        bool pre_threatened_src = false;
        if (depth >= 3 && !in_chk && !is_cap && moves_searched >= 1 && !is_tt && !is_killer && !is_tactical_score) {
            pre_threatened_src = get_src_attacked(m.src);
            if (!pre_threatened_src) {
                if ((pfeats.koma_search || pfeats.koma_search_fast) && !get_may_give_chk()) {
                    pre_bad_koma_quiet = is_bad_koma_quiet(pos, m, stm);
                }
                if ((root_ply_ + ply) <= 16 && m.kind == MoveKind::Rotate) {
                    pre_passive_rotate = is_passive_rotate(pos, m, enemy, enemy_king);
                }
            }
        }

        int move_ext = 0;
        int next_ext_budget = ext_budget;
        int next_double_ext = double_ext;

        // Singular / Double / Multi-Cut Extension on TT move (no Negative Extension at shallow CLP depths)
        if (ply > 0 && ply < 2 * current_root_depth_ && depth >= 7 && excluded_move.is_none() &&
            use_staged && !in_chk && is_tt && tt_hit &&
            std::abs(tt_score) < SCORE_SUPERIOR - 200 && tte.depth >= depth - 3 &&
            (tte.bound == TT_LOWER || tte.bound == TT_EXACT)) {
            int s_beta = tt_score - (sp.se_margin_x2 * depth) / 2;
            int s_depth = (depth - 1) / 2;
            int16_t saved_eval = eval_stack_[ply];
            bool se_rep = false;
            int s_val = pvs(pos, s_depth, s_beta - 1, s_beta, ply, false, 0, false,
                            prev_move, prev2_move, prev1_f, prev2_f, m, &se_rep, 0, rep_count,
                            cut_node, double_ext);
            eval_stack_[ply] = saved_eval;
            if (abort_) return 0;
            if (se_rep) node_rep_tainted = true;

            if (s_val < s_beta) {
                if (next_ext_budget > 0) {
                    move_ext = 1;
                    --next_ext_budget;
                    if (!is_pv && s_val < s_beta - 35 && double_ext < 3 && next_ext_budget > 0) {
                        move_ext = 2;
                        --next_ext_budget;
                        ++next_double_ext;
                    }
                }
            } else if (!is_pv && s_beta >= beta && std::abs(s_beta) < SCORE_SUPERIOR - 200 && !se_rep) {
                if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
                return s_beta;
            }
        }

        move_stack_[ply] = m;
        move_f_stack_[ply] = curr_end_f;
        move_ptsq_stack_[ply] = (end_sq >= 0 && end_sq < NUM_SQUARES) ? static_cast<int16_t>(ptsq_idx(m.ptype, end_sq)) : int16_t(-1);
        move_pfsq_stack_[ply] = (end_sq >= 0 && end_sq < NUM_SQUARES) ? static_cast<int16_t>(pfsq_idx(m.ptype, curr_end_f, end_sq)) : int16_t(-1);
        reduction_stack_[ply] = 0;

        UndoState u;
        apply_move(pos, m, u);
        nnue_invalidate(ply + 1);
        tt_ptr_->prefetch(pos.zobrist);
        ++nodes_;
#ifdef HOMEI_STATS
        ++stats_.nodes;
#endif
        if ((nodes_ & 511ULL) == 0 && check_stop()) {
            undo_move(pos, m, u);
            return 0;
        }
        record_path_node(pos, ply + 1);

        ++moves_searched;
        if (!is_cap) {
            ++quiet_searched;
            if (n_quiet_tried < 64) {
                quiet_tried[n_quiet_tried] = m;
                quiet_tried_f[n_quiet_tried] = curr_end_f;
                ++n_quiet_tried;
            }
        } else if (n_cap_tried < 32) {
            cap_tried[n_cap_tried] = m;
            cap_tried_vic[n_cap_tried] = cap_vic_pt;
            ++n_cap_tried;
        }

        int score = 0;
        int child_rep_cnt = count_repetitions(pos.zobrist, ply + 1);
        if (child_rep_cnt >= 3) {
            node_rep_tainted = true;
            undo_move(pos, m, u);
            pv_length_[ply + 1] = ply + 1;
            if (seen_non_rep3 || has_any_non_rep3_move(pos, ply)) {
                seen_non_rep3 = true;
                score = -(SCORE_MATE - (ply + 1)); // Điều 4: Thiên Nhật Thủ (thua)
            } else {
                score = 0; // Điều 5: Thiên Nhật Hòa (hòa bất khả kháng)
            }
        } else {
            seen_non_rep3 = true;
            bool gives_chk = is_in_check(pos, enemy);
            int8_t child_in_chk = gives_chk ? 1 : 0;
            int new_depth = std::max(0, depth - 1 + move_ext);
            bool child_rep = false;

            if (moves_searched == 1) {
                reduction_stack_[ply] = 0;
                score = -pvs(pos, new_depth, -beta, -alpha, ply + 1, is_pv, next_ext_budget, true,
                             m, prev_move, curr_end_f, prev1_f, Move::none(), &child_rep, child_in_chk, child_rep_cnt,
                             is_pv ? false : !cut_node, next_double_ext);
            } else {
                // Calibrated 1024-Scale LMR (fractional adjustments for CLP: cut_node, fam_rank, 1/2/4-ply continuous history, mild PV LMR at ply >= 3)
                int reduction = 0;
                bool lmr_exempt = is_cap || gives_chk || in_chk || is_tt || is_killer || pre_threatened_src || is_tactical_score;
                bool lmr_eligible = (!is_pv && depth >= 3 && moves_searched >= 2) ||
                                    (is_pv && ply >= 3 && depth >= 5 && moves_searched >= 5);
                if (lmr_eligible && !in_chk && !lmr_exempt) {
                    int d_idx = std::min(depth, 63);
                    int m_idx = std::min(moves_searched, 127);
                    int r = lmr_table_1024[d_idx][m_idx];

                    if (is_pv) r -= 1280;
                    if (cut_node) r += 512;
                    if (!improving) r += 768;
                    if (tt_capture) r += 384;
                    if (mate_threat) r -= 1024;
                    if (curr_fam_rank == 1) r += 384;
                    else if (curr_fam_rank >= 2) r += 768;
                    if (pre_bad_koma_quiet) r += 768;
                    if (pre_passive_rotate) r += 512;
                    if (is_worker_) r += ((thread_id_ & 1) ? 128 : -128);

                    int total_hist = get_move_total_hist();
                    r -= std::clamp((total_hist * 1024) / 16000, -1536, 1536);

                    reduction = std::clamp(r / 1024, 0, std::max(0, new_depth - 1));
                } else if (!is_pv && depth >= 4 && !in_chk && is_cap && move_order_score < -100000 && !gives_chk && !is_tt && moves_searched >= 2) {
                    reduction = std::min(1, std::max(0, new_depth - 1));
                }

                int reduced_depth = std::max(0, new_depth - reduction);
                reduction_stack_[ply] = static_cast<int8_t>(std::max(0, reduction));
                bool zw_cut = (reduction > 0) ? true : !cut_node;
                score = -pvs(pos, reduced_depth, -(alpha + 1), -alpha, ply + 1, false, next_ext_budget, true,
                             m, prev_move, curr_end_f, prev1_f, Move::none(), &child_rep, child_in_chk, child_rep_cnt,
                             zw_cut, next_double_ext);
                if (score > alpha && reduction > 0 && !abort_) {
#ifdef HOMEI_STATS
                    ++stats_.lmr_researches;
#endif
                    int re_depth = new_depth;
                    if (new_depth >= 5 && best_score > -SCORE_SUPERIOR && std::abs(score) < SCORE_SUPERIOR - 200) {
                        if (score > best_score + 60 + 2 * new_depth) ++re_depth;
                    }
                    reduction_stack_[ply] = 0;
                    score = -pvs(pos, re_depth, -(alpha + 1), -alpha, ply + 1, false, next_ext_budget, true,
                                 m, prev_move, curr_end_f, prev1_f, Move::none(), &child_rep, child_in_chk, child_rep_cnt,
                                 !cut_node, next_double_ext);
                }
                if (score > alpha && score < beta && is_pv && !abort_) {
                    reduction_stack_[ply] = 0;
                    score = -pvs(pos, new_depth, -beta, -alpha, ply + 1, true, next_ext_budget, true,
                                 m, prev_move, curr_end_f, prev1_f, Move::none(), &child_rep, child_in_chk, child_rep_cnt,
                                 false, next_double_ext);
                }
            }

            undo_move(pos, m, u);
            if (child_rep) node_rep_tainted = true;
            if (abort_) return 0;
        }

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
                    if (is_cap) {
                        int cbonus = std::min(4000, depth * depth * 12);
                        if (m.dst >= 0 && m.dst < NUM_SQUARES) {
                            update_hist_entry(cap_history_[stm][m.ptype][cap_vic_pt][m.dst], cbonus);
                        }
                        for (int ci = 0; ci < n_cap_tried - 1; ++ci) {
                            const Move& pm = cap_tried[ci];
                            if (pm.dst >= 0 && pm.dst < NUM_SQUARES) {
                                update_hist_entry(cap_history_[stm][pm.ptype][cap_tried_vic[ci]][pm.dst], -cbonus / 2);
                            }
                        }
                    } else if (ply < MAX_PLY) {
                        int hbonus = std::min(4000, depth * depth * 12);
                        int cbonus = std::min(1600, depth * depth * 5);
                        if (m.kind != MoveKind::Drop) {
                            if (killers_[ply][0] != m) {
                                killers_[ply][1] = killers_[ply][0];
                                killers_[ply][0] = m;
                            }
                            if (end_sq >= 0 && end_sq < NUM_SQUARES) {
                                update_hist_entry(history_[stm][m.ptype][curr_end_f][end_sq], hbonus);
                            }
                        } else if (pfeats.drop_history) {
                            if (killers_[ply][0] != m) {
                                killers_[ply][1] = killers_[ply][0];
                                killers_[ply][0] = m;
                            }
                            if (m.dst >= 0 && m.dst < NUM_SQUARES) {
                                update_hist_entry(drop_history_[stm][m.ptype][curr_end_f][m.dst], hbonus);
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

                        // Decaying Quiet Malus on earlier failed quiet moves (early failures punished harder!)
                        for (int qi = 0; qi < n_quiet_tried - 1; ++qi) {
                            const Move& qm = quiet_tried[qi];
                            int8_t qf = quiet_tried_f[qi];
                            int qsq = (qm.kind == MoveKind::Rotate) ? qm.src : qm.dst;
                            if (qsq < 0 || qsq >= NUM_SQUARES) continue;
                            int denom = 4 + (qi >> 1);
                            int q_malus = -(hbonus * 3) / denom;
                            int c_malus = -(cbonus * 3) / denom;
                            if (qm.kind != MoveKind::Drop) {
                                update_hist_entry(history_[stm][qm.ptype][qf][qsq], q_malus);
                            } else if (pfeats.drop_history) {
                                update_hist_entry(drop_history_[stm][qm.ptype][qf][qsq], q_malus);
                            }
                            update_5ply_cont_hist(ply, qm.ptype, qf, qsq, c_malus);
                        }

                        // Thread-Local 5-Ply Continuation History Bonus (1/2/3/4/6-ply, zero atomic_ref!)
                        update_5ply_cont_hist(ply, m.ptype, curr_end_f, end_sq, cbonus);
                    }
                    break;
                }
            }
        }
    }

    if (moves_searched == 0) {
        if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
        if (!excluded_move.is_none()) return alpha;
        // PAT or checkmate (no legal moves existed in pos)
        return -(SCORE_MATE - ply);
    }

    uint8_t bound = (best_score <= orig_alpha) ? TT_UPPER : ((best_score >= beta) ? TT_LOWER : TT_EXACT);

    // Fail-Low Prior-Move Bonus: reward opponent's move that caused this node to fail low
    if (bound == TT_UPPER && depth >= 3 && !prev_move.is_none() && ply >= 1 && !abort_) {
        int prev_end_sq = (prev_move.kind == MoveKind::Rotate) ? prev_move.src : prev_move.dst;
        if (prev_end_sq >= 0 && prev_end_sq < NUM_SQUARES) {
            int fl_bonus = std::min(1200, depth * depth * 4);
            if (prev_move.kind != MoveKind::Drop) {
                update_hist_entry(history_[enemy][prev_move.ptype][prev1_f][prev_end_sq], fl_bonus);
            } else if (pfeats.drop_history) {
                update_hist_entry(drop_history_[enemy][prev_move.ptype][prev1_f][prev_end_sq], fl_bonus);
            }
            update_5ply_cont_hist(ply - 1, prev_move.ptype, prev1_f, prev_end_sq, fl_bonus / 2);
        }
    }

    // Update 5-Table L2-resident Correction History (strictly on EXACT non-lazy, quiet, non-tainted, non-mate nodes!)
    if (!in_chk && excluded_move.is_none() && rep_count == 1 && !node_rep_tainted && !abort_ &&
        raw_static_eval != EVAL_NONE && std::abs(best_score) < SCORE_SUPERIOR - 200 &&
        (best_move.is_none() || !is_capture_move(pos, best_move))) {
        if (bound == TT_EXACT ||
            (bound == TT_LOWER && best_score > static_eval) ||
            (bound == TT_UPPER && best_score < static_eval)) {
            update_correction_history(pos, ply, depth, best_score - static_eval);
        }
    }

    if (excluded_move.is_none() && rep_count == 1 && !node_rep_tainted && !abort_) {
        tt_ptr_->store(pos.zobrist, depth, ply, best_score, bound, best_move, raw_static_eval);
    }

    if (out_rep_tainted && node_rep_tainted) *out_rep_tainted = true;
    return best_score;
}

void Searcher::cache_mate_pv(const Position& start_pos, const std::vector<Move>& pv, bool root_is_attacker) {
    if (pv.empty()) return;
    Position sim = start_pos;
    const int L = static_cast<int>(pv.size());
    for (int k = 0; k < L; ++k) {
        const Move& m = pv[k];
        if (m.is_none()) break;
        const bool is_atk = root_is_attacker ? ((k & 1) == 0) : ((k & 1) == 1);
        const int rem_ply = L - k;
        const int sc = is_atk ? (SCORE_MATE - rem_ply) : -(SCORE_MATE - rem_ply);
        if (is_atk) {
            tt_ptr_->store(sim.zobrist, 30, 0, sc, TT_LOWER, m);
        }

        int slot = -1;
        for (size_t i = 0; i < MATE_CACHE_SIZE; ++i) {
            if (mate_cache_[i].valid && mate_cache_[i].hash == sim.zobrist) {
                slot = static_cast<int>(i);
                break;
            }
        }
        if (slot < 0) {
            slot = static_cast<int>(mate_cache_next_ % MATE_CACHE_SIZE);
            ++mate_cache_next_;
        }
        CachedMateNode& cn = mate_cache_[slot];
        if (!cn.valid || cn.hash != sim.zobrist || cn.is_attacker != is_atk || rem_ply <= cn.mate_ply) {
            cn.hash = sim.zobrist;
            cn.valid = true;
            cn.is_attacker = is_atk;
            cn.mate_ply = rem_ply;
            cn.best_move = m;
            cn.pv.assign(pv.begin() + k, pv.end());
        }

        UndoState u;
        apply_move(sim, m, u);
    }
}

const Searcher::CachedMateNode* Searcher::lookup_mate_cache(uint64_t hash) const {
    for (size_t i = 0; i < MATE_CACHE_SIZE; ++i) {
        if (mate_cache_[i].valid && mate_cache_[i].hash == hash) {
            return &mate_cache_[i];
        }
    }
    return nullptr;
}

SearchResult Searcher::search(
    const GameState& state,
    const SearchLimits& limits,
    std::atomic<bool>& stop_flag,
    const InfoCallback& on_info
) {
#ifdef HOMEI_PROFILE
    SearchProfile& prof = get_search_profile();
    const bool prof_on = prof.enabled;
    std::chrono::steady_clock::time_point t_search0;
    if (prof_on) {
        t_search0 = std::chrono::steady_clock::now();
    }
    struct TotalProfGuard {
        bool on;
        std::chrono::steady_clock::time_point t0;
        ~TotalProfGuard() {
            if (on) {
                auto t1 = std::chrono::steady_clock::now();
                get_search_profile().total_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()
                );
            }
        }
    } tguard{prof_on, t_search0};
#endif

    SearchResult res;
    limits_ = limits;
    stop_flag_ = &stop_flag;
    root_has_result_ = false;
    abort_ = false;
    nodes_ = 0;
    live_nodes_atomic_.store(0, std::memory_order_relaxed);
#ifdef HOMEI_STATS
    stats_.reset();
#endif
    start_time_ms_ = now_ms();
    if (clear_dfpn_pending_) {
        clear_dfpn_tt();
        clear_dfpn_pending_ = false;
    }

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
            Bitboard kb = root_pos.bb_side[s] & root_pos.bb_type[VUONG];
            if (kb.any()) root_pos.king_sq[s] = static_cast<uint8_t>(kb.lsb());
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
    rebuild_base_rep_bloom();

    record_path_node(root_pos, 0);
    const bool age_tables = (state.ply > last_search_ply_ && last_search_ply_ >= 0 && !limits_.infinite);
    if (age_tables) {
        age_ordering_tables();
    } else {
        clear_ordering_tables();
    }
    if (last_search_ply_ >= 0 && state.ply + 4 < last_search_ply_ && !lookup_mate_cache(root_pos.zobrist)) {
        clear_dfpn_tt();
    }
    last_search_ply_ = state.ply;
    root_ply_ = state.ply;
    nnue_prepare_root();
    std::fill_n(eval_stack_, MAX_PLY + 4, EVAL_NONE);
    {
        bool dummy_lazy = false;
        eval_stack_[0] = static_cast<int16_t>(std::clamp(eval_node(root_pos, 0, -SCORE_INFINITE, SCORE_INFINITE, false, dummy_lazy), -30000, 30000));
    }
    std::fill_n(move_ptsq_stack_, MAX_PLY + 4, int16_t(-1));
    std::fill_n(move_pfsq_stack_, MAX_PLY + 4, int16_t(-1));
    std::memset(reduction_stack_, 0, sizeof(reduction_stack_));
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

    bool has_rep2_base = false;
    for (const auto& b : mate_rep_) {
        if (b.count >= 2) {
            has_rep2_base = true;
            break;
        }
    }
    const bool can_reuse_dfpn_tt = !has_rep2_base;

    Side root_stm = static_cast<Side>(root_pos.side_to_move);
    Side root_enemy = other_side(root_stm);
    int ek = root_pos.king_sq[root_enemy];
    bool enemy_in_chk = is_in_check(root_pos, root_enemy);
    bool own_in_chk = is_in_check(root_pos, root_stm);
    bool kz_attacked = false;
    int enemy_escapes = 0;
    if (ek < NUM_SQUARES && !enemy_in_chk) {
        int ekr = sq_row(ek), ekc = sq_col(ek);
        for (int dr = -1; dr <= 1 && !kz_attacked; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                int nr = ekr + dr, nc = ekc + dc;
                if (!in_bounds(nr, nc)) continue;
                if (is_attacked(root_pos, make_sq(nr, nc), root_stm)) {
                    kz_attacked = true;
                    break;
                }
            }
        }
        Bitboard ek_dsts = BB_STEP_ATTACKS[VUONG][0][ek] & ~root_pos.bb_side[root_enemy];
        while (ek_dsts.any()) {
            int sq = ek_dsts.pop_lsb();
            if (!is_attacked(root_pos, sq, root_stm)) {
                ++enemy_escapes;
            }
        }
    }
    bool stm_has_prisoner = false;
    if (root_pos.reserve[root_stm] == 0) {
        for (int pt = 1; pt < 8; ++pt) {
            if (root_pos.prisoners[root_stm][pt] > 0) { stm_has_prisoner = true; break; }
        }
    }

    struct RootMateEntry {
        Move m = Move::none();
        bool known = false;
        bool is_win = false;
        bool proved_shortest = false;
        int disproved_lim = 0;
        int mate_ply = 0;
        std::vector<Move> pv;
        int score() const { return is_win ? (SCORE_MATE - mate_ply) : -(SCORE_MATE - mate_ply); }
    };
    std::vector<RootMateEntry> root_mates;
    root_mates.reserve(static_cast<size_t>(std::min(root_legal.count, 64)));

    auto find_root_mate = [&](const Move& m) -> RootMateEntry* {
        for (auto& e : root_mates) {
            if (e.m == m) return &e;
        }
        return nullptr;
    };

    auto record_root_mate = [&](const Move& m, bool is_win, int mate_ply, bool proved_shortest, const std::vector<Move>& pv) {
        RootMateEntry* e = find_root_mate(m);
        if (!e) {
            root_mates.emplace_back();
            e = &root_mates.back();
            e->m = m;
        }
        if (!e->known || is_win != e->is_win || mate_ply < e->mate_ply ||
            (mate_ply == e->mate_ply && (pv.size() > e->pv.size() || (proved_shortest && !e->proved_shortest)))) {
            e->known = true;
            e->is_win = is_win;
            e->mate_ply = mate_ply;
            e->proved_shortest = proved_shortest;
            e->pv = pv;
        } else if (proved_shortest) {
            e->proved_shortest = true;
        }
    };

    auto mark_root_disproved = [&](const Move& m, int lim) {
        RootMateEntry* e = find_root_mate(m);
        if (!e) {
            root_mates.emplace_back();
            e = &root_mates.back();
            e->m = m;
        }
        if (lim > e->disproved_lim) {
            e->disproved_lim = lim;
        }
    };

    auto promote_root_move = [&](const Move& target, int boost_score) {
        for (int ri = 0; ri < root_legal.count; ++ri) {
            if (root_legal.moves[ri] == target) {
                Move tmp = root_legal.moves[ri];
                int sc = (boost_score != 0) ? boost_score : root_scores[ri];
                for (int k = ri; k > 0; --k) {
                    root_legal.moves[k] = root_legal.moves[k - 1];
                    root_scores[k] = root_scores[k - 1];
                }
                root_legal.moves[0] = tmp;
                root_scores[0] = sc;
                break;
            }
        }
    };

    auto verify_terminal_mate_pv = [&](const Position& start, const std::vector<Move>& pv, bool root_wins) -> bool {
        if (pv.empty()) return false;
        Position sim = start;
        std::vector<uint64_t> hashes;
        hashes.reserve(pv.size() + 1);
        hashes.push_back(sim.zobrist);
        for (size_t k = 0; k < pv.size(); ++k) {
            MoveList leg;
            generate_legal_moves(sim, leg);
            bool ok = false;
            for (int li = 0; li < leg.count; ++li) {
                if (leg.moves[li] == pv[k]) {
                    ok = true;
                    break;
                }
            }
            if (!ok) return false;
            UndoState su;
            apply_move(sim, pv[k], su);
            int rep_c = 1;
            for (const auto& br : base_rep_) {
                if (br.hash == sim.zobrist) {
                    rep_c += br.count;
                    break;
                }
            }
            for (size_t hi = (base_rep_.empty() ? 0 : 1); hi < hashes.size(); ++hi) {
                if (hashes[hi] == sim.zobrist) ++rep_c;
            }
            hashes.push_back(sim.zobrist);
            if (rep_c >= 3) {
                if (k + 1 != pv.size()) return false;
                // Điều 5 (Thiên Nhật Hòa): if EVERY legal move from previous state led to rep3, it's a DRAW, not a forced mate!
                undo_move(sim, pv[k], su);
                bool any_non_rep3 = false;
                for (int li = 0; li < leg.count; ++li) {
                    if (leg.moves[li] == pv[k]) continue;
                    UndoState cu;
                    apply_move(sim, leg.moves[li], cu);
                    int alt_c = 1;
                    for (const auto& br : base_rep_) {
                        if (br.hash == sim.zobrist) {
                            alt_c += br.count;
                            break;
                        }
                    }
                    for (size_t hi = (base_rep_.empty() ? 0 : 1); hi + 1 < hashes.size(); ++hi) {
                        if (hashes[hi] == sim.zobrist) ++alt_c;
                    }
                    undo_move(sim, leg.moves[li], cu);
                    if (alt_c < 3) {
                        any_non_rep3 = true;
                        break;
                    }
                }
                if (!any_non_rep3) return false;
                bool mover_is_root = ((k & 1) == 0);
                return root_wins ? !mover_is_root : mover_is_root;
            }
        }
        MoveList end_leg;
        generate_legal_moves(sim, end_leg);
        if (end_leg.count != 0) return false;
        bool stm_is_root = ((pv.size() & 1) == 0);
        return root_wins ? !stm_is_root : stm_is_root;
    };

    bool any_root_win_mate = false;
    bool all_root_moves_mated = false;
    bool defense_mate_hint = false;
    int defense_safe_up_to = 0;
    bool has_mate_signal = false;
    bool phase2_atk_done = false;
    bool phase3_atk_done = false;

    std::atomic<uint64_t> dfpn_live_nodes{0};
    auto sync_dfpn_nodes = [&](uint64_t added_nodes) {
        nodes_ += added_nodes;
        live_nodes_atomic_.store(nodes_, std::memory_order_relaxed);
        dfpn_live_nodes.store(0, std::memory_order_relaxed);
    };

    // Step A: Probe persistent Mate-Line Cache for instant continuity across consecutive GUI moves
    if (EngineFeatures::get().mate_root && can_reuse_dfpn_tt && limits_.max_nodes >= 1000) {
        const CachedMateNode* cached = lookup_mate_cache(root_pos.zobrist);
        if (cached && verify_terminal_mate_pv(root_pos, cached->pv, cached->is_attacker)) {
            if (cached->is_attacker) {
                bool recorded = false;
                if (limits_.max_nodes >= 40000 && max_d >= 6 &&
                    (limits_.infinite || limits_.movetime_ms <= 0 || hard_limit_ms_ >= 60)) {
                    MateResult cm = solve_mate(
                        root_pos, cached->mate_ply, 60, 40000ULL, stop_flag_, &mate_rep_, true, true,
                        nullptr, 0, &dfpn_live_nodes
                    );
                    sync_dfpn_nodes(cm.nodes);
                    if (cm.found && !cm.pv.empty()) {
                        record_root_mate(cm.pv.front(), true, cm.mate_ply, cm.proved_shortest, cm.pv);
                        promote_root_move(cm.pv.front(), 10000000);
                        if (!cm.rep_dependent) {
                            tt_ptr_->store(root_pos.zobrist, 30, 0, SCORE_MATE - cm.mate_ply, cm.proved_shortest ? TT_EXACT : TT_LOWER, cm.pv.front());
                            cache_mate_pv(root_pos, cm.pv, true);
                        }
                        any_root_win_mate = true;
                        recorded = true;
                    }
                }
                if (!recorded) {
                    record_root_mate(cached->pv.front(), true, cached->mate_ply, false, cached->pv);
                    promote_root_move(cached->pv.front(), 10000000);
                    tt_ptr_->store(root_pos.zobrist, 30, 0, SCORE_MATE - cached->mate_ply, TT_LOWER, cached->pv.front());
                    any_root_win_mate = true;
                }
            } else {
                defense_mate_hint = true;
                record_root_mate(cached->pv.front(), false, cached->mate_ply, false, cached->pv);
                promote_root_move(cached->pv.front(), 500000);
            }
        }
    }

    // Tier 1: Fast Phase 1 df-pn at Root (plus Phase 2 before ID only in non-infinite movetime mode)
    if (EngineFeatures::get().mate_root && limits_.max_nodes >= 1000 &&
        (limits_.infinite || limits_.movetime_ms <= 0 || hard_limit_ms_ >= 40)) {

        bool run_mate_phase1 = enemy_in_chk || own_in_chk || kz_attacked || stm_has_prisoner;
        if (run_mate_phase1 && !any_root_win_mate) {
            int64_t phase1_budget = (limits_.infinite || limits_.movetime_ms <= 0)
                ? 50
                : std::clamp<int64_t>(hard_limit_ms_ / 6, 12, 35);
            int mate_max_ply = std::min(17, max_d);
            uint64_t phase1_nodes = std::min<uint64_t>(3000ULL, limits_.max_nodes / 10);
            MateResult mate_res = solve_mate(
                root_pos, mate_max_ply, phase1_budget, phase1_nodes, stop_flag_, &mate_rep_, true, can_reuse_dfpn_tt,
                nullptr, 0, &dfpn_live_nodes
            );
            sync_dfpn_nodes(mate_res.nodes);

            if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) {
                res.best_move = root_legal.moves[0];
                res.total_nodes = nodes_;
                res.elapsed_ms = now_ms() - start_time_ms_;
                return res;
            }

            if (!mate_res.found) {
                has_mate_signal = (kz_attacked && enemy_escapes <= 4) ||
                                  (mate_res.root_pn <= 20 && (kz_attacked || stm_has_prisoner)) ||
                                  enemy_in_chk;

                if (!limits_.infinite && has_mate_signal && limits_.max_nodes >= 60000 && max_d >= 6 &&
                    (limits_.movetime_ms <= 0 || hard_limit_ms_ >= 60)) {
                    int64_t phase2_budget = (limits_.movetime_ms >= 2000)
                        ? std::min<int64_t>(1500, hard_limit_ms_ / 2)
                        : std::min<int64_t>(450, std::max<int64_t>(20, hard_limit_ms_ / 3));
                    int deep_ply = (limits_.movetime_ms >= 2000) ? 41 : 31;
                    uint64_t phase2_nodes = std::min<uint64_t>((limits_.movetime_ms >= 2000) ? 1000000ULL : 180000ULL, limits_.max_nodes);
                    mate_res = solve_mate(
                        root_pos, deep_ply, phase2_budget, phase2_nodes, stop_flag_, &mate_rep_, true, can_reuse_dfpn_tt,
                        nullptr, 0, &dfpn_live_nodes
                    );
                    sync_dfpn_nodes(mate_res.nodes);
                    phase2_atk_done = true;
                }
            }

            if (stop_flag_ && stop_flag_->load(std::memory_order_relaxed)) {
                res.best_move = root_legal.moves[0];
                res.total_nodes = nodes_;
                res.elapsed_ms = now_ms() - start_time_ms_;
                return res;
            }

            if (mate_res.found && !mate_res.pv.empty()) {
                record_root_mate(mate_res.pv.front(), true, mate_res.mate_ply, mate_res.proved_shortest, mate_res.pv);
                promote_root_move(mate_res.pv.front(), 10000000);
                if (!mate_res.rep_dependent) {
                    tt_ptr_->store(root_pos.zobrist, 30, 0, SCORE_MATE - mate_res.mate_ply, mate_res.proved_shortest ? TT_EXACT : TT_LOWER, mate_res.pv.front());
                    cache_mate_pv(root_pos, mate_res.pv, true);
                }
                any_root_win_mate = true;
            }
        }
    }

    // Fast path for single-PV movetime matches / tactical gates (T28, etc.)
    if (any_root_win_mate && multipv_k == 1 && !limits_.infinite) {
        const RootMateEntry* best_win = nullptr;
        for (const auto& e : root_mates) {
            if (e.known && e.is_win && (!best_win || e.mate_ply < best_win->mate_ply)) {
                best_win = &e;
            }
        }
        if (best_win && !best_win->pv.empty()) {
            PVLine initial_line;
            initial_line.multipv_rank = 1;
            initial_line.depth = std::min(max_d, std::max(1, best_win->mate_ply));
            initial_line.score = best_win->score();
            initial_line.nodes = nodes_;
            initial_line.time_ms = std::max<int64_t>(1, now_ms() - start_time_ms_);
            initial_line.pv = best_win->pv;
            if (on_info) {
                on_info(initial_line);
            }
            res.lines = {initial_line};
            res.best_move = best_win->m;
            res.total_nodes = nodes_;
            res.elapsed_ms = now_ms() - start_time_ms_;
            return res;
        }
    }

    int prev_score = 0;
    Move last_best_moves[3] = {Move::none(), Move::none(), Move::none()};
    int last_scores[3] = {0, 0, 0};

    // Root attack-mate refinement helper (tightens mate length toward shortest forced mate)
    auto refine_root_attack_mate = [&](int64_t budget_ms, uint64_t max_nodes_cap) -> bool {
        if (!EngineFeatures::get().mate_root || !any_root_win_mate) return false;
        RootMateEntry* best_win = nullptr;
        for (auto& e : root_mates) {
            if (e.known && e.is_win && (!best_win || e.mate_ply < best_win->mate_ply)) {
                best_win = &e;
            }
        }
        if (!best_win || best_win->proved_shortest || best_win->mate_ply <= 1) return false;
        if (nodes_ >= limits_.max_nodes || check_stop()) return false;

        int target_ply = (best_win->mate_ply & 1) ? (best_win->mate_ply - 2) : (best_win->mate_ply - 1);
        if (target_ply < 1) {
            best_win->proved_shortest = true;
            return false;
        }
        uint64_t cap = std::min(max_nodes_cap, limits_.max_nodes - nodes_);
        if (cap < 10000) return false;

        MateResult sh = solve_mate(
            root_pos, target_ply, budget_ms, cap, stop_flag_, &mate_rep_, true, can_reuse_dfpn_tt,
            nullptr, 0, &dfpn_live_nodes
        );
        sync_dfpn_nodes(sh.nodes);
        if (sh.found && !sh.pv.empty() && sh.mate_ply < best_win->mate_ply) {
            record_root_mate(sh.pv.front(), true, sh.mate_ply, sh.proved_shortest, sh.pv);
            promote_root_move(sh.pv.front(), 10000000);
            if (!sh.rep_dependent) {
                tt_ptr_->store(root_pos.zobrist, 30, 0, SCORE_MATE - sh.mate_ply, sh.proved_shortest ? TT_EXACT : TT_LOWER, sh.pv.front());
                cache_mate_pv(root_pos, sh.pv, true);
            }
            return true;
        } else if (!sh.aborted && sh.root_dn == 0) {
            best_win->proved_shortest = true;
            if (!has_rep2_base) {
                tt_ptr_->store(root_pos.zobrist, 30, 0, best_win->score(), TT_EXACT, best_win->m);
            }
        }
        return false;
    };

    // MultiPV secondary attacking mate solver helper
    auto run_multipv_attack_mates = [&](int64_t total_budget_ms, uint64_t per_pv_nodes, int deep_ply) {
        if (!EngineFeatures::get().mate_root || multipv_k <= 1 || limits_.max_nodes < 60000) return;
        std::vector<Move> excluded_wins;
        excluded_wins.reserve(static_cast<size_t>(multipv_k));
        for (const auto& e : root_mates) {
            if (e.known && e.is_win) excluded_wins.push_back(e.m);
        }
        int64_t p_start = now_ms();
        while (static_cast<int>(excluded_wins.size()) < multipv_k && !check_stop()) {
            int64_t rem_ms = total_budget_ms - (now_ms() - p_start);
            if (rem_ms < 40) break;
            if (nodes_ >= limits_.max_nodes) break;
            uint64_t cap_nodes = std::min(per_pv_nodes, limits_.max_nodes - nodes_);
            if (cap_nodes < 10000) break;
            int64_t slice_ms = std::min<int64_t>(rem_ms, 500);
            MateResult extra = solve_mate(
                root_pos,
                deep_ply,
                slice_ms,
                cap_nodes,
                stop_flag_,
                &mate_rep_,
                true,
                can_reuse_dfpn_tt,
                excluded_wins.empty() ? nullptr : excluded_wins.data(),
                static_cast<int>(excluded_wins.size()),
                &dfpn_live_nodes
            );
            sync_dfpn_nodes(extra.nodes);
            if (extra.found && !extra.pv.empty()) {
                record_root_mate(extra.pv.front(), true, extra.mate_ply, extra.proved_shortest, extra.pv);
                if (!extra.rep_dependent) cache_mate_pv(root_pos, extra.pv, true);
                excluded_wins.push_back(extra.pv.front());
            } else {
                break;
            }
        }
    };

    // Root defense mate solver helper (proves whether ALL legal moves get mated, choosing longest defense)
    auto run_defense_mate_pass = [&](int64_t total_budget_ms, uint64_t per_move_nodes, int def_max_ply, bool refine_unproved = false) {
        if (!EngineFeatures::get().mate_root || any_root_win_mate) return;
        if (!refine_unproved && (all_root_moves_mated || defense_safe_up_to >= def_max_ply)) return;
        if (limits_.max_nodes < 60000 || max_d < 6) return;

        // If neither in check nor warned by cache/heavy eval deficit, only probe a small top subset tightly
        const bool urgent = own_in_chk || defense_mate_hint || (prev_score <= -1500) || refine_unproved;
        int check_limit = root_legal.count;
        if (!own_in_chk && root_legal.count > 32) {
            if (!urgent && root_legal.count > 48) {
                check_limit = std::min(root_legal.count, std::max(multipv_k + 4, 8));
            } else if (root_legal.count > 48) {
                check_limit = 48;
            }
        }

        int64_t pass_start = now_ms();
        bool all_checked_mated = true;
        std::vector<RepHashEntry> child_rep = mate_rep_;
        child_rep.push_back({0ULL, 0});

        for (int i = 0; i < check_limit; ++i) {
            if (check_stop() || (now_ms() - pass_start) >= total_budget_ms) {
                all_checked_mated = false;
                break;
            }
            if (nodes_ >= limits_.max_nodes) {
                all_checked_mated = false;
                break;
            }

            const Move m = root_legal.moves[i];
            RootMateEntry* existing = find_root_mate(m);
            if (existing && existing->known && !existing->is_win && existing->proved_shortest) {
                continue;
            }
            if (refine_unproved && all_root_moves_mated && existing && existing->known && !existing->is_win && existing->mate_ply < 5) {
                existing->proved_shortest = true;
                continue;
            }
            if (!refine_unproved && existing && existing->disproved_lim >= def_max_ply) {
                defense_safe_up_to = std::max(defense_safe_up_to, existing->disproved_lim);
                all_checked_mated = false;
                break;
            }

            Position child = root_pos;
            UndoState u;
            apply_move(child, m, u);

            int rep_cnt = 1;
            for (const auto& br : base_rep_) {
                if (br.hash == child.zobrist) {
                    rep_cnt += br.count;
                    break;
                }
            }
            if (rep_cnt >= 3) {
                // Check Điều 5 (Thiên Nhật Hòa): if EVERY legal root move leads to rep3, it's a draw, NOT a loss!
                if (!has_any_non_rep3_move(root_pos, 0)) {
                    all_checked_mated = false;
                    break;
                }
                // Otherwise Điều 4: this move immediately triggers 3-fold repetition loss in 1 ply
                std::vector<Move> rp = {m};
                record_root_mate(m, false, 1, true, rp);
                continue;
            }

            // Update trailing child entry in child_rep
            child_rep.pop_back();
            bool merged_in_base = false;
            bool child_has_rep2 = has_rep2_base;
            for (size_t bi = 0; bi < child_rep.size(); ++bi) {
                if (child_rep[bi].hash == child.zobrist) {
                    child_rep[bi].count += 1;
                    if (child_rep[bi].count >= 2) child_has_rep2 = true;
                    merged_in_base = true;
                    break;
                }
            }
            if (!merged_in_base) {
                child_rep.push_back({child.zobrist, 1});
            }

            int64_t rem_ms = total_budget_ms - (now_ms() - pass_start);
            if (rem_ms < 25) {
                all_checked_mated = false;
                if (merged_in_base) {
                    for (size_t bi = 0; bi < child_rep.size(); ++bi) {
                        if (child_rep[bi].hash == child.zobrist) {
                            child_rep[bi].count -= 1;
                            break;
                        }
                    }
                }
                break;
            }
            int64_t mv_budget = urgent
                ? std::min<int64_t>(rem_ms, 1000)
                : std::min<int64_t>(rem_ms, (i == 0 ? 350 : 600));
            uint64_t mv_nodes = urgent ? per_move_nodes : std::min<uint64_t>(per_move_nodes, (i == 0 ? 100000ULL : per_move_nodes));
            mv_nodes = std::min(mv_nodes, limits_.max_nodes - nodes_);
            if (mv_nodes < 5000) {
                all_checked_mated = false;
                break;
            }

            int query_ply = def_max_ply;
            if (refine_unproved && existing && existing->known && !existing->is_win && existing->mate_ply >= 4) {
                int opp_cur = existing->mate_ply - 1;
                int want = (opp_cur & 1) ? (opp_cur - 2) : (opp_cur - 1);
                if (want >= 1) query_ply = std::min(def_max_ply, want);
            }

            MateResult opp = solve_mate(
                child,
                query_ply,
                mv_budget,
                mv_nodes,
                stop_flag_,
                &child_rep,
                true,
                !child_has_rep2,
                nullptr,
                0,
                &dfpn_live_nodes
            );
            sync_dfpn_nodes(opp.nodes);

            // Restore child_rep back to mate_rep_ + placeholder for next iteration
            if (merged_in_base) {
                for (size_t bi = 0; bi < child_rep.size(); ++bi) {
                    if (child_rep[bi].hash == child.zobrist) {
                        child_rep[bi].count -= 1;
                        break;
                    }
                }
                child_rep.push_back({0ULL, 0});
            }

            if (opp.found && !opp.pv.empty()) {
                int def_ply = opp.mate_ply + 1;
                std::vector<Move> full_pv;
                full_pv.reserve(1 + opp.pv.size());
                full_pv.push_back(m);
                full_pv.insert(full_pv.end(), opp.pv.begin(), opp.pv.end());
                record_root_mate(m, false, def_ply, opp.proved_shortest, full_pv);
                if (!opp.rep_dependent && !child_has_rep2) {
                    cache_mate_pv(child, opp.pv, true);
                }
            } else if (!opp.aborted && opp.root_dn == 0) {
                if (existing && existing->known && !existing->is_win && query_ply < existing->mate_ply - 1) {
                    // Proved NO strictly shorter mate exists -> existing->mate_ply IS shortest!
                    existing->proved_shortest = true;
                } else {
                    mark_root_disproved(m, query_ply);
                    defense_safe_up_to = std::max(defense_safe_up_to, query_ply);
                    all_checked_mated = false;
                    break;
                }
            } else {
                all_checked_mated = false;
                if (!limits_.infinite || (i == 0 && !refine_unproved)) {
                    break;
                }
            }
        }

        if ((all_checked_mated && check_limit == root_legal.count) || all_root_moves_mated) {
            bool verified_all = true;
            for (int i = 0; i < root_legal.count; ++i) {
                const RootMateEntry* rme = find_root_mate(root_legal.moves[i]);
                if (!rme || !rme->known || rme->is_win) {
                    verified_all = false;
                    break;
                }
            }
            if (verified_all) {
                all_root_moves_mated = true;
                // Stable bubble sort root_legal & root_scores by losing mate_ply descending (longest defense first)
                for (int a = 0; a < root_legal.count; ++a) {
                    for (int b = a + 1; b < root_legal.count; ++b) {
                        const RootMateEntry* ra = find_root_mate(root_legal.moves[a]);
                        const RootMateEntry* rb = find_root_mate(root_legal.moves[b]);
                        if (rb->mate_ply > ra->mate_ply) {
                            std::swap(root_legal.moves[a], root_legal.moves[b]);
                            std::swap(root_scores[a], root_scores[b]);
                        }
                    }
                }
                const RootMateEntry* best_def = find_root_mate(root_legal.moves[0]);
                if (best_def && !best_def->pv.empty()) {
                    tt_ptr_->store(root_pos.zobrist, 30, 0, best_def->score(), TT_EXACT, best_def->m);
                    if (can_reuse_dfpn_tt) {
                        cache_mate_pv(root_pos, best_def->pv, false);
                    }
                }
            }
        }
    };

    bool multipv_atk_d2_done = false;
    bool multipv_atk_d4_done = false;
    bool defense_d2_done = false;
    bool defense_d4_done = false;

    const Position immutable_root_pos = root_pos;
    const SearchLimits worker_limits = limits_;
    std::atomic<bool> worker_stop{false};
    bool workers_dispatched = false;

    ensure_workers();
    if (!workers_.empty()) {
        for (size_t idx = 0; idx < workers_.size(); ++idx) {
            Searcher* w = workers_[idx].get();
            w->set_shared_tt(tt_ptr_);
            w->dispatch_job(immutable_root_pos, worker_limits, worker_stop, base_rep_, mate_rep_, state.ply, age_tables);
        }
        workers_dispatched = true;
    }

    std::mutex info_mutex;
    std::mutex emit_mutex;
    std::condition_variable hb_cv;
    bool hb_stop = false;
    std::vector<PVLine> hb_lines;
    std::atomic<uint64_t> info_seq{0};
    int64_t last_emit_ms = start_time_ms_;
    uint64_t last_emitted_nodes = 0;

    auto compute_live_nodes = [&]() -> uint64_t {
        uint64_t tot_n = live_nodes_atomic_.load(std::memory_order_relaxed)
                       + dfpn_live_nodes.load(std::memory_order_relaxed);
        for (const auto& w : workers_) {
            tot_n += w->live_nodes_atomic_.load(std::memory_order_relaxed);
        }
        if (tot_n < last_emitted_nodes) tot_n = last_emitted_nodes;
        last_emitted_nodes = tot_n;
        return tot_n;
    };

    std::thread hb_thread;
    struct SearchThreadsGuard {
        std::atomic<bool>& w_stop;
        std::vector<std::unique_ptr<Searcher>>& workers_ref;
        bool& dispatched_ref;
        std::mutex& info_mtx;
        std::condition_variable& hb_cv_ref;
        bool& hb_stop_ref;
        std::thread& hb_th;
        void shutdown() {
            w_stop.store(true, std::memory_order_relaxed);
            if (hb_th.joinable()) {
                {
                    std::lock_guard<std::mutex> lk(info_mtx);
                    hb_stop_ref = true;
                }
                hb_cv_ref.notify_all();
                hb_th.join();
            }
            if (dispatched_ref) {
                for (auto& w : workers_ref) {
                    if (w) w->wait_job_done();
                }
                dispatched_ref = false;
            }
        }
        ~SearchThreadsGuard() { shutdown(); }
    } threads_guard{worker_stop, workers_, workers_dispatched, info_mutex, hb_cv, hb_stop, hb_thread};

    if (limits_.infinite && on_info) {
        hb_thread = std::thread([&]() {
            while (true) {
                std::vector<PVLine> snap;
                uint64_t snap_seq = 0;
                {
                    std::unique_lock<std::mutex> lk(info_mutex);
                    hb_cv.wait_for(lk, std::chrono::milliseconds(200), [&]() { return hb_stop; });
                    if (hb_stop || stop_flag.load(std::memory_order_relaxed)) break;
                    if (hb_lines.empty()) continue;
                    int64_t now = now_ms();
                    if (now - last_emit_ms < 180) continue;
                    uint64_t tot_n = compute_live_nodes();
                    int64_t elapsed = std::max<int64_t>(1, now - start_time_ms_);
                    snap = hb_lines;
                    snap_seq = info_seq.load(std::memory_order_acquire);
                    for (auto& ln : snap) {
                        ln.time_ms = elapsed;
                        ln.nodes = tot_n;
                    }
                    last_emit_ms = now;
                }
                std::lock_guard<std::mutex> elk(emit_mutex);
                if (info_seq.load(std::memory_order_acquire) != snap_seq) continue;
                for (const auto& ln : snap) {
                    on_info(ln);
                }
            }
        });
    }

    for (int d = 1; d <= max_d; ++d) {
        current_root_depth_ = d;
        std::vector<Move> excluded_first;
        excluded_first.reserve(multipv_k);
        std::vector<PVLine> depth_lines;
        depth_lines.reserve(multipv_k);

        bool depth_aborted = false;

        const bool use_aspiration = (!any_root_win_mate && !all_root_moves_mated && root_mates.empty() &&
                                     multipv_k == 1 && d >= 5 && std::abs(prev_score) < 2000);
        if (use_aspiration) {
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

                    if ((d > 1 || root_has_result_) && check_stop()) {
                        depth_aborted = true;
                        break;
                    }

                    int8_t mf = move_end_facing(root_pos, m);
                    int m_end_sq = (m.kind == MoveKind::Rotate) ? m.src : m.dst;
                    move_stack_[0] = m;
                    move_f_stack_[0] = mf;
                    move_ptsq_stack_[0] = (m_end_sq >= 0 && m_end_sq < NUM_SQUARES) ? static_cast<int16_t>(ptsq_idx(m.ptype, m_end_sq)) : int16_t(-1);
                    move_pfsq_stack_[0] = (m_end_sq >= 0 && m_end_sq < NUM_SQUARES) ? static_cast<int16_t>(pfsq_idx(m.ptype, mf, m_end_sq)) : int16_t(-1);
                    reduction_stack_[0] = 0;
                    UndoState u;
                    apply_move(root_pos, m, u);
                    nnue_invalidate(1);
                    ++nodes_;
#ifdef HOMEI_STATS
                    ++stats_.nodes;
#endif
                    record_path_node(root_pos, 1);

                    Side enemy = static_cast<Side>(root_pos.side_to_move);
                    bool gives_chk = is_in_check(root_pos, enemy);
                    int ext = (d == 1 && gives_chk) ? 1 : 0;
                    int ext_budget = 4;

                    int score = 0;
                    if (count_repetitions(root_pos.zobrist, 1) >= 3) {
                        undo_move(root_pos, m, u);
                        pv_length_[1] = 1;
                        score = has_any_non_rep3_move(root_pos, 0) ? -(SCORE_MATE - 1) : 0;
                    } else {
                        if (searched_in_rank == 0) {
                            score = -pvs(root_pos, d - 1 + ext, -beta, -alpha, 1, true, ext_budget, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, false, 0);
                        } else {
                            int root_red = (d >= 3 && searched_in_rank >= 8 && !gives_chk && root_scores[i] < 180000) ? 1 : 0;
                            reduction_stack_[0] = static_cast<int8_t>(root_red);
                            score = -pvs(root_pos, std::max(0, d - 1 + ext - root_red), -(alpha + 1), -alpha, 1, false, ext_budget, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, true, 0);
                            if (score > alpha && root_red > 0 && !abort_) {
                                reduction_stack_[0] = 0;
                                score = -pvs(root_pos, d - 1 + ext, -(alpha + 1), -alpha, 1, false, ext_budget, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, true, 0);
                            }
                            if (score > alpha && score < beta && !abort_) {
                                reduction_stack_[0] = 0;
                                score = -pvs(root_pos, d - 1 + ext, -beta, -alpha, 1, true, ext_budget, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, false, 0);
                            }
                        }

                        undo_move(root_pos, m, u);
                    }
                    ++searched_in_rank;

                    if (abort_ && (d > 1 || root_has_result_)) {
                        depth_aborted = true;
                        break;
                    }

                    if (score > best_score || best_m.is_none()) {
                        best_score = score;
                        best_m = m;
                        if (d == 1) root_has_result_ = true;
                        best_pv.clear();
                        best_pv.push_back(m);
                        for (int k = 1; k < pv_length_[1]; ++k) {
                            best_pv.push_back(pv_table_[1][k]);
                        }
                        if (score > alpha) {
                            alpha = score;
                            if (alpha >= beta) {
                                break;
                            }
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
                        promote_root_move(best_m, 0);
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
                        promote_root_move(best_m, 0);
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
            for (int pv_idx = 0; pv_idx < multipv_k; ++pv_idx) {
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

                    if ((d > 1 || root_has_result_) && check_stop()) {
                        depth_aborted = true;
                        break;
                    }

                    RootMateEntry* rmi = find_root_mate(m);
                    int score = 0;
                    std::vector<Move> cand_pv;
                    bool used_pvs = false;

                    if (rmi && rmi->known && rmi->is_win) {
                        int ms = rmi->score();
                        if (!best_m.is_none() && (best_score > ms || (best_score == ms && d >= 5))) {
                            continue;
                        }
                        // For already DF-PN-proved wins, skip heavy PVS at d >= 5 if proved shortest or already beaten;
                        // at d <= 4 run quick PVS to catch any shorter non-checking Hisshi mate.
                        if (d <= 4 || (!rmi->proved_shortest && rmi->mate_ply <= d + 2)) {
                            used_pvs = true;
                            int8_t mf = move_end_facing(root_pos, m);
                            int m_end_sq = (m.kind == MoveKind::Rotate) ? m.src : m.dst;
                            move_stack_[0] = m;
                            move_f_stack_[0] = mf;
                            move_ptsq_stack_[0] = (m_end_sq >= 0 && m_end_sq < NUM_SQUARES) ? static_cast<int16_t>(ptsq_idx(m.ptype, m_end_sq)) : int16_t(-1);
                            move_pfsq_stack_[0] = (m_end_sq >= 0 && m_end_sq < NUM_SQUARES) ? static_cast<int16_t>(pfsq_idx(m.ptype, mf, m_end_sq)) : int16_t(-1);
                            reduction_stack_[0] = 0;
                            UndoState u;
                            apply_move(root_pos, m, u);
                            nnue_invalidate(1);
                            ++nodes_;
#ifdef HOMEI_STATS
                            ++stats_.nodes;
#endif
                            record_path_node(root_pos, 1);
                            Side enemy = static_cast<Side>(root_pos.side_to_move);
                            bool gives_chk = is_in_check(root_pos, enemy);
                            int ext = (d == 1 && gives_chk) ? 1 : 0;
                            int ab_sc = -pvs(root_pos, d - 1 + ext, -beta, -alpha, 1, true, 4, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, false, 0);
                            undo_move(root_pos, m, u);
                            if (abort_ && (d > 1 || root_has_result_)) {
                                depth_aborted = true;
                                break;
                            }
                            if (ab_sc > ms && ab_sc >= SCORE_MATE - MAX_PLY) {
                                cand_pv.push_back(m);
                                for (int k = 1; k < pv_length_[1]; ++k) cand_pv.push_back(pv_table_[1][k]);
                                if (verify_terminal_mate_pv(root_pos, cand_pv, true)) {
                                    int new_ply = static_cast<int>(cand_pv.size());
                                    if (new_ply < rmi->mate_ply) {
                                        record_root_mate(m, true, new_ply, true, cand_pv);
                                        rmi = find_root_mate(m);
                                        ms = rmi->score();
                                        if (can_reuse_dfpn_tt) cache_mate_pv(root_pos, rmi->pv, true);
                                    }
                                }
                            }
                        }
                        score = rmi->score();
                        cand_pv = rmi->pv;
                    } else if (rmi && rmi->known && !rmi->is_win) {
                        int ms = rmi->score();
                        // Upper bound pruning: cannot beat a move already scoring >= ms
                        if (!best_m.is_none() && best_score >= ms) {
                            continue;
                        }
                        // Check PVS at shallow depths (d <= 5) or near mate depth to detect faster non-checking losses
                        if (!all_root_moves_mated || d <= 5 || (!rmi->proved_shortest && rmi->mate_ply <= d + 2)) {
                            used_pvs = true;
                            int8_t mf = move_end_facing(root_pos, m);
                            int m_end_sq = (m.kind == MoveKind::Rotate) ? m.src : m.dst;
                            move_stack_[0] = m;
                            move_f_stack_[0] = mf;
                            move_ptsq_stack_[0] = (m_end_sq >= 0 && m_end_sq < NUM_SQUARES) ? static_cast<int16_t>(ptsq_idx(m.ptype, m_end_sq)) : int16_t(-1);
                            move_pfsq_stack_[0] = (m_end_sq >= 0 && m_end_sq < NUM_SQUARES) ? static_cast<int16_t>(pfsq_idx(m.ptype, mf, m_end_sq)) : int16_t(-1);
                            reduction_stack_[0] = 0;
                            UndoState u;
                            apply_move(root_pos, m, u);
                            nnue_invalidate(1);
                            ++nodes_;
#ifdef HOMEI_STATS
                            ++stats_.nodes;
#endif
                            record_path_node(root_pos, 1);
                            Side enemy = static_cast<Side>(root_pos.side_to_move);
                            bool gives_chk = is_in_check(root_pos, enemy);
                            int ext = (d == 1 && gives_chk) ? 1 : 0;
                            int child_alpha = -ms;
                            int child_beta = (alpha > -SCORE_INFINITE && alpha < ms) ? -alpha : SCORE_INFINITE;
                            if (d <= 2) {
                                child_alpha = -beta;
                                child_beta = -alpha;
                            }
                            int ab_sc = -pvs(root_pos, d - 1 + ext, child_alpha, child_beta, 1, (searched_in_rank == 0), 4, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, (searched_in_rank != 0), 0);
                            undo_move(root_pos, m, u);
                            if (abort_ && (d > 1 || root_has_result_)) {
                                depth_aborted = true;
                                break;
                            }
                            if (ab_sc < ms && ab_sc <= -(SCORE_MATE - MAX_PLY)) {
                                std::vector<Move> ab_pv;
                                ab_pv.push_back(m);
                                for (int k = 1; k < pv_length_[1]; ++k) ab_pv.push_back(pv_table_[1][k]);
                                if (verify_terminal_mate_pv(root_pos, ab_pv, false)) {
                                    int new_ply = static_cast<int>(ab_pv.size());
                                    if (new_ply < rmi->mate_ply) {
                                        record_root_mate(m, false, new_ply, true, ab_pv);
                                        rmi = find_root_mate(m);
                                    }
                                }
                            }
                        }
                        score = rmi->score();
                        cand_pv = rmi->pv;
                    } else {
                        used_pvs = true;
                        int8_t mf = move_end_facing(root_pos, m);
                        int m_end_sq = (m.kind == MoveKind::Rotate) ? m.src : m.dst;
                        move_stack_[0] = m;
                        move_f_stack_[0] = mf;
                        move_ptsq_stack_[0] = (m_end_sq >= 0 && m_end_sq < NUM_SQUARES) ? static_cast<int16_t>(ptsq_idx(m.ptype, m_end_sq)) : int16_t(-1);
                        move_pfsq_stack_[0] = (m_end_sq >= 0 && m_end_sq < NUM_SQUARES) ? static_cast<int16_t>(pfsq_idx(m.ptype, mf, m_end_sq)) : int16_t(-1);
                        reduction_stack_[0] = 0;
                        UndoState u;
                        apply_move(root_pos, m, u);
                        nnue_invalidate(1);
                        ++nodes_;
#ifdef HOMEI_STATS
                        ++stats_.nodes;
#endif
                        record_path_node(root_pos, 1);

                        Side enemy = static_cast<Side>(root_pos.side_to_move);
                        bool gives_chk = is_in_check(root_pos, enemy);
                        int ext = (d == 1 && gives_chk) ? 1 : 0;
                        int ext_budget = 4;

                        if (count_repetitions(root_pos.zobrist, 1) >= 3) {
                            undo_move(root_pos, m, u);
                            pv_length_[1] = 1;
                            score = has_any_non_rep3_move(root_pos, 0) ? -(SCORE_MATE - 1) : 0;
                        } else {
                            if (searched_in_rank == 0) {
                                score = -pvs(root_pos, d - 1 + ext, -beta, -alpha, 1, true, ext_budget, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, false, 0);
                            } else {
                                int root_red = (d >= 3 && searched_in_rank >= 8 && !gives_chk && root_scores[i] < 180000) ? 1 : 0;
                                reduction_stack_[0] = static_cast<int8_t>(root_red);
                                score = -pvs(root_pos, std::max(0, d - 1 + ext - root_red), -(alpha + 1), -alpha, 1, false, ext_budget, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, true, 0);
                                if (score > alpha && root_red > 0 && !abort_) {
                                    reduction_stack_[0] = 0;
                                    score = -pvs(root_pos, d - 1 + ext, -(alpha + 1), -alpha, 1, false, ext_budget, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, true, 0);
                                }
                                if (score > alpha && score < beta && !abort_) {
                                    reduction_stack_[0] = 0;
                                    score = -pvs(root_pos, d - 1 + ext, -beta, -alpha, 1, true, ext_budget, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, false, 0);
                                }
                            }

                            undo_move(root_pos, m, u);
                        }
                        if (abort_ && (d > 1 || root_has_result_)) {
                            depth_aborted = true;
                            break;
                        }

                        // Record Alpha-Beta mate discovery directly into root_mates
                        if (std::abs(score) >= SCORE_MATE - MAX_PLY) {
                            std::vector<Move> ab_pv;
                            ab_pv.push_back(m);
                            for (int k = 1; k < pv_length_[1]; ++k) ab_pv.push_back(pv_table_[1][k]);
                            if (verify_terminal_mate_pv(root_pos, ab_pv, score > 0)) {
                                bool win = (score > 0);
                                int mply = static_cast<int>(ab_pv.size());
                                record_root_mate(m, win, mply, true, ab_pv);
                                if (win) any_root_win_mate = true;
                                RootMateEntry* new_e = find_root_mate(m);
                                score = new_e->score();
                                cand_pv = new_e->pv;
                            } else {
                                cand_pv = std::move(ab_pv);
                            }
                        } else {
                            cand_pv.push_back(m);
                            for (int k = 1; k < pv_length_[1]; ++k) {
                                cand_pv.push_back(pv_table_[1][k]);
                            }
                        }
                    }

                    if (used_pvs) {
                        ++searched_in_rank;
                    } else {
                        ++searched_in_rank;
                    }

                    if (score > best_score || best_m.is_none()) {
                        best_score = score;
                        best_m = m;
                        if (d == 1) root_has_result_ = true;
                        best_pv = std::move(cand_pv);
                        if (score > alpha) {
                            alpha = score;
                        }
                    }
                }

                if (depth_aborted && best_m.is_none()) {
                    break;
                }
                if (!best_m.is_none()) {
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
            std::stable_sort(depth_lines.begin(), depth_lines.end(), [](const PVLine& a, const PVLine& b) {
                return a.score > b.score;
            });
            {
                live_nodes_atomic_.store(nodes_, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lk(info_mutex);
                uint64_t tot_n = compute_live_nodes();
                int64_t elapsed = std::max<int64_t>(1, now_ms() - start_time_ms_);
                for (size_t idx = 0; idx < depth_lines.size(); ++idx) {
                    depth_lines[idx].multipv_rank = static_cast<int>(idx + 1);
                    depth_lines[idx].nodes = tot_n;
                    depth_lines[idx].time_ms = elapsed;
                }
                hb_lines = depth_lines;
                info_seq.fetch_add(1, std::memory_order_release);
                last_emit_ms = now_ms();
            }
            if (on_info) {
                std::lock_guard<std::mutex> elk(emit_mutex);
                for (const auto& dl : depth_lines) {
                    on_info(dl);
                }
            }
            res.lines = depth_lines;
            res.best_move = depth_lines[0].pv.front();

            // Promote best moves to front of root_legal for next iteration
            for (int idx = static_cast<int>(depth_lines.size()) - 1; idx >= 0; --idx) {
                promote_root_move(depth_lines[idx].pv.front(), 0);
            }
            prev_score = depth_lines[0].score;

            // Progressive DF-PN deep search, MultiPV mate probing, and Defense Mate verification
            // run AFTER emitting d == 1 / d == 2 / d == 3 so GUI gets instant shallow info and Stop responds immediately.
            if (EngineFeatures::get().mate_root && !abort_ && !depth_aborted) {
                if (d == 1 && multipv_k >= 3 && limits_.infinite && !any_root_win_mate && has_mate_signal && !phase2_atk_done &&
                    limits_.max_nodes >= 60000 && max_d >= 6) {
                    MateResult p2 = solve_mate(
                        root_pos, 41, 1800, std::min<uint64_t>(900000ULL, limits_.max_nodes),
                        stop_flag_, &mate_rep_, true, can_reuse_dfpn_tt,
                        nullptr, 0, &dfpn_live_nodes
                    );
                    sync_dfpn_nodes(p2.nodes);
                    phase2_atk_done = true;
                    if (p2.found && !p2.pv.empty()) {
                        record_root_mate(p2.pv.front(), true, p2.mate_ply, p2.proved_shortest, p2.pv);
                        promote_root_move(p2.pv.front(), 10000000);
                        if (!p2.rep_dependent) {
                            tt_ptr_->store(root_pos.zobrist, 30, 0, SCORE_MATE - p2.mate_ply, p2.proved_shortest ? TT_EXACT : TT_LOWER, p2.pv.front());
                            cache_mate_pv(root_pos, p2.pv, true);
                        }
                        any_root_win_mate = true;

                        // Immediately publish the discovered forced mate as PV1 at depth 1,
                        // keeping distinct d=1 alternative lines for multipv 2..k.
                        std::vector<PVLine> updated;
                        updated.reserve(static_cast<size_t>(multipv_k));
                        PVLine mate_line;
                        mate_line.multipv_rank = 1;
                        mate_line.depth = d;
                        mate_line.score = SCORE_MATE - p2.mate_ply;
                        mate_line.pv = p2.pv;
                        updated.push_back(std::move(mate_line));
                        for (const auto& dl : depth_lines) {
                            if (static_cast<int>(updated.size()) >= multipv_k) break;
                            if (!dl.pv.empty() && dl.pv.front() == p2.pv.front()) continue;
                            updated.push_back(dl);
                        }
                        {
                            live_nodes_atomic_.store(nodes_, std::memory_order_relaxed);
                            std::lock_guard<std::mutex> lk(info_mutex);
                            uint64_t tot_n = compute_live_nodes();
                            int64_t elapsed = std::max<int64_t>(1, now_ms() - start_time_ms_);
                            for (size_t idx = 0; idx < updated.size(); ++idx) {
                                updated[idx].multipv_rank = static_cast<int>(idx + 1);
                                updated[idx].nodes = tot_n;
                                updated[idx].time_ms = elapsed;
                            }
                            hb_lines = updated;
                            info_seq.fetch_add(1, std::memory_order_release);
                            last_emit_ms = now_ms();
                        }
                        if (on_info) {
                            std::lock_guard<std::mutex> elk(emit_mutex);
                            for (const auto& dl : updated) {
                                on_info(dl);
                            }
                        }
                        res.lines = updated;
                        res.best_move = updated[0].pv.front();
                        prev_score = updated[0].score;
                    }
                } else if (d == 2) {
                    if (limits_.infinite && !any_root_win_mate && has_mate_signal && !phase2_atk_done &&
                        limits_.max_nodes >= 60000 && max_d >= 6) {
                        MateResult p2 = solve_mate(
                            root_pos, 41, 1800, std::min<uint64_t>(900000ULL, limits_.max_nodes),
                            stop_flag_, &mate_rep_, true, can_reuse_dfpn_tt,
                            nullptr, 0, &dfpn_live_nodes
                        );
                        sync_dfpn_nodes(p2.nodes);
                        phase2_atk_done = true;
                        if (p2.found && !p2.pv.empty()) {
                            record_root_mate(p2.pv.front(), true, p2.mate_ply, p2.proved_shortest, p2.pv);
                            promote_root_move(p2.pv.front(), 10000000);
                            if (!p2.rep_dependent) {
                                tt_ptr_->store(root_pos.zobrist, 30, 0, SCORE_MATE - p2.mate_ply, p2.proved_shortest ? TT_EXACT : TT_LOWER, p2.pv.front());
                                cache_mate_pv(root_pos, p2.pv, true);
                            }
                            any_root_win_mate = true;
                        }
                    }
                    if (any_root_win_mate && !check_stop()) {
                        if (limits_.infinite) {
                            if ((now_ms() - start_time_ms_) < 500) {
                                refine_root_attack_mate(1000, 800000ULL);
                            }
                        } else if (limits_.movetime_ms >= 2000) {
                            int64_t rem_t = hard_limit_ms_ - (now_ms() - start_time_ms_);
                            if (rem_t >= 200) {
                                refine_root_attack_mate(std::min<int64_t>(rem_t / 2, 800), 600000ULL);
                            }
                        }
                    }
                    if (any_root_win_mate && multipv_k > 1 && !multipv_atk_d2_done) {
                        if (!limits_.infinite || (now_ms() - start_time_ms_) < 500) {
                            int64_t rem_time = limits_.infinite ? 1000 : std::max<int64_t>(0, (hard_limit_ms_ - (now_ms() - start_time_ms_)) / 3);
                            if (rem_time >= 80) {
                                run_multipv_attack_mates(std::min<int64_t>(rem_time, 1000), 250000ULL, 41);
                            }
                            multipv_atk_d2_done = true;
                        }
                    }
                    if (!any_root_win_mate && !all_root_moves_mated && !defense_d2_done &&
                        (defense_mate_hint || own_in_chk || prev_score <= -600)) {
                        int64_t def_budget = limits_.infinite
                            ? 2500
                            : std::min<int64_t>(1000, std::max<int64_t>(0, (hard_limit_ms_ - (now_ms() - start_time_ms_)) / 2));
                        if (def_budget >= 80) {
                            run_defense_mate_pass(def_budget, 600000ULL, 41, false);
                        }
                        defense_d2_done = true;
                    }
                } else if (d == 3 && limits_.infinite) {
                    if (!any_root_win_mate && has_mate_signal && !phase3_atk_done && limits_.max_nodes >= 200000) {
                        MateResult p3 = solve_mate(
                            root_pos, 41, 1800, std::min<uint64_t>(1500000ULL, limits_.max_nodes),
                            stop_flag_, &mate_rep_, true, can_reuse_dfpn_tt,
                            nullptr, 0, &dfpn_live_nodes
                        );
                        sync_dfpn_nodes(p3.nodes);
                        phase3_atk_done = true;
                        if (p3.found && !p3.pv.empty()) {
                            record_root_mate(p3.pv.front(), true, p3.mate_ply, p3.proved_shortest, p3.pv);
                            promote_root_move(p3.pv.front(), 10000000);
                            if (!p3.rep_dependent) {
                                tt_ptr_->store(root_pos.zobrist, 30, 0, SCORE_MATE - p3.mate_ply, p3.proved_shortest ? TT_EXACT : TT_LOWER, p3.pv.front());
                                cache_mate_pv(root_pos, p3.pv, true);
                            }
                            any_root_win_mate = true;
                        }
                    }
                    bool refined = false;
                    if (any_root_win_mate && !check_stop()) {
                        refined = refine_root_attack_mate(1500, 1200000ULL);
                    }
                    if (any_root_win_mate && multipv_k > 1 && !multipv_atk_d4_done && !refined &&
                        (now_ms() - start_time_ms_) < 2200) {
                        run_multipv_attack_mates(800, 300000ULL, 41);
                        multipv_atk_d4_done = true;
                    }
                    if (!any_root_win_mate && !defense_d4_done &&
                        (all_root_moves_mated || defense_mate_hint || own_in_chk || prev_score <= -500)) {
                        run_defense_mate_pass(3000, 900000ULL, 41, all_root_moves_mated);
                        defense_d4_done = true;
                    }
                }
            }

            // Clock mode soft-stop around optimum_time with dynamic extension on score drop / bestmove instability
            if (limits_.optimum_time_ms > 0 && !limits_.infinite) {
                int64_t elapsed = now_ms() - start_time_ms_;
                Move curr_best = depth_lines[0].pv.front();
                int curr_score = depth_lines[0].score;

                bool stable = (d >= 4 && !last_best_moves[0].is_none() && curr_best == last_best_moves[0] && curr_best == last_best_moves[1]);
                bool score_stable = (d >= 4 && std::abs(curr_score - last_scores[0]) <= 25 && std::abs(curr_score - last_scores[1]) <= 25);
                bool unstable = (d >= 3 && !last_best_moves[0].is_none() && (curr_best != last_best_moves[0] || (last_scores[0] - curr_score) > 40));

                int64_t soft_limit = optimum_time;
                if (unstable) {
                    soft_limit = std::min<int64_t>(hard_limit_ms_, (optimum_time * 18) / 10);
                } else if (d >= 5 && stable && score_stable && std::abs(curr_score) < SCORE_MATE - 100) {
                    soft_limit = (optimum_time * 55) / 100;
                }

                last_best_moves[2] = last_best_moves[1];
                last_best_moves[1] = last_best_moves[0];
                last_best_moves[0] = curr_best;
                last_scores[2] = last_scores[1];
                last_scores[1] = last_scores[0];
                last_scores[0] = curr_score;

                if (d >= 3 && elapsed >= soft_limit) {
                    break;
                }
            }
        }

        if (abort_ || depth_aborted) break;
        if (!limits_.infinite && nodes_ >= limits_.max_nodes) break;
        if (!limits_.infinite && all_root_moves_mated && d >= 4) {
            bool all_sh = true;
            for (int i = 0; i < root_legal.count; ++i) {
                const RootMateEntry* rme = find_root_mate(root_legal.moves[i]);
                if (!rme || !rme->proved_shortest) { all_sh = false; break; }
            }
            if (all_sh || d >= 8) break;
        }
        if (!limits_.infinite && any_root_win_mate && d >= 4 &&
            static_cast<int>(depth_lines.size()) == multipv_k) {
            bool all_lines_mate = true;
            for (const auto& dl : depth_lines) {
                if (std::abs(dl.score) < SCORE_MATE - MAX_PLY) { all_lines_mate = false; break; }
            }
            if (all_lines_mate && d >= 8) break;
        }
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

    if (limits_.infinite && !stop_flag.load(std::memory_order_relaxed)) {
        worker_stop.store(true, std::memory_order_relaxed);
        while (!stop_flag.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    threads_guard.shutdown();
    for (const auto& w : workers_) {
        nodes_ += w->nodes_;
    }

    // Best-Thread Voting (YaneuraOu/Stockfish SMP: select best move across all completed threads)
    if (multipv_k == 1 && !any_root_win_mate && !all_root_moves_mated &&
        !workers_.empty() && !res.lines.empty() && !res.lines[0].pv.empty()) {
        struct VoteCand {
            Move move;
            int depth;
            int score;
            bool is_main;
            const std::vector<Move>* pv;
        };
        std::vector<VoteCand> cands;
        cands.reserve(1 + workers_.size());
        cands.push_back({res.lines[0].pv.front(), res.lines[0].depth, res.lines[0].score, true, &res.lines[0].pv});
        for (const auto& w : workers_) {
            if (!w || w->voted_depth_ < 2 || w->voted_move_.is_none()) continue;
            bool is_leg = false;
            for (int ri = 0; ri < root_legal.count; ++ri) {
                if (root_legal.moves[ri] == w->voted_move_) { is_leg = true; break; }
            }
            if (!is_leg) continue;
            cands.push_back({w->voted_move_, w->voted_depth_, w->voted_score_, false, &w->voted_pv_});
        }
        if (cands.size() > 1) {
            int winner_idx = -1;
            // 1. Prefer verified shortest forced winning mate if any thread found one
            int best_mate_score = -SCORE_INFINITE;
            for (size_t ci = 0; ci < cands.size(); ++ci) {
                if (cands[ci].score >= SCORE_MATE - MAX_PLY && cands[ci].pv && !cands[ci].pv->empty()) {
                    if (verify_terminal_mate_pv(root_pos, *cands[ci].pv, true) && cands[ci].score > best_mate_score) {
                        best_mate_score = cands[ci].score;
                        winner_idx = static_cast<int>(ci);
                    }
                }
            }
            // 2. Otherwise weighted Stockfish voting: weight += (score - min_score + 10) * depth
            if (winner_idx < 0) {
                int min_sc = cands[0].score;
                for (size_t ci = 1; ci < cands.size(); ++ci) {
                    if (cands[ci].score < min_sc) min_sc = cands[ci].score;
                }
                int64_t best_votes = -1;
                Move winning_move = cands[0].move;
                for (size_t ci = 0; ci < cands.size(); ++ci) {
                    const Move mv = cands[ci].move;
                    int64_t v = 0;
                    for (size_t cj = 0; cj < cands.size(); ++cj) {
                        if (cands[cj].move == mv) {
                            int64_t w_sc = std::clamp<int64_t>(static_cast<int64_t>(cands[cj].score) - min_sc, 0, 10000);
                            v += (w_sc + 10) * static_cast<int64_t>(cands[cj].depth) + (cands[cj].is_main ? 1 : 0);
                        }
                    }
                    if (v > best_votes) {
                        best_votes = v;
                        winning_move = mv;
                    }
                }
                // Pick deepest/highest-scoring candidate advocating winning_move
                for (size_t ci = 0; ci < cands.size(); ++ci) {
                    if (cands[ci].move != winning_move) continue;
                    if (winner_idx < 0 ||
                        cands[ci].depth > cands[winner_idx].depth ||
                        (cands[ci].depth == cands[winner_idx].depth && cands[ci].score > cands[winner_idx].score)) {
                        winner_idx = static_cast<int>(ci);
                    }
                }
            }
            if (winner_idx > 0 && cands[winner_idx].move != res.best_move) {
                const VoteCand& win = cands[winner_idx];
                std::vector<Move> clean_pv;
                if (win.pv && !win.pv->empty()) {
                    Position sim = root_pos;
                    for (const Move& pm : *win.pv) {
                        MoveList sim_legal;
                        generate_legal_moves(sim, sim_legal);
                        bool found = false;
                        for (int li = 0; li < sim_legal.count; ++li) {
                            if (sim_legal.moves[li] == pm) { found = true; break; }
                        }
                        if (!found) break;
                        clean_pv.push_back(pm);
                        UndoState su;
                        apply_move(sim, pm, su);
                    }
                }
                if (clean_pv.empty()) clean_pv.push_back(win.move);
                res.best_move = win.move;
                res.lines[0].depth = std::max(res.lines[0].depth, win.depth);
                res.lines[0].score = win.score;
                res.lines[0].nodes = nodes_;
                res.lines[0].time_ms = std::max<int64_t>(1, now_ms() - start_time_ms_);
                res.lines[0].pv = std::move(clean_pv);
                if (on_info) {
                    on_info(res.lines[0]);
                }
            }
        }
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
    live_nodes_atomic_.store(0, std::memory_order_relaxed);
    start_time_ms_ = now_ms();
    hard_limit_ms_ = 1000000000LL;
    voted_depth_ = 0;
    voted_score_ = 0;
    voted_move_ = Move::none();
    voted_pv_.clear();
    nnue_prepare_root();
    std::fill_n(eval_stack_, MAX_PLY + 4, EVAL_NONE);
    {
        bool dummy_lazy = false;
        eval_stack_[0] = static_cast<int16_t>(std::clamp(eval_node(pos, 0, -SCORE_INFINITE, SCORE_INFINITE, false, dummy_lazy), -30000, 30000));
    }
    std::fill_n(move_ptsq_stack_, MAX_PLY + 4, int16_t(-1));
    std::fill_n(move_pfsq_stack_, MAX_PLY + 4, int16_t(-1));
    std::memset(reduction_stack_, 0, sizeof(reduction_stack_));

    MoveList& legal = ply_stack_[0].moves;
    int* scores = ply_stack_[0].scores;
    generate_legal_moves(pos, legal);
    if (legal.count == 0) return;

    record_path_node(pos, 0);
    int max_d = std::clamp(limits_.max_depth, 1, 32);

    score_moves(pos, legal, scores, Move::none(), 0, Move::none(), Move::none(), Move::none());
    for (int i = 0; i < legal.count; ++i) {
        scores[i] += ((thread_id * 131 + i * 17) & 31) - 16;
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
    }

    int prev_score = 0;
    int start_d = 1 + (thread_id & 1);
    int asp_base = 25 + (thread_id % 3) * 10;
    for (int d = start_d; d <= max_d; ++d) {
        if (d >= 5 && thread_id >= 2 && ((d + thread_id) % 3 == 0)) continue;
        current_root_depth_ = d;

        int delta = (d >= 5 && std::abs(prev_score) < 2000) ? asp_base : SCORE_INFINITE;
        int asp_alpha = (delta == SCORE_INFINITE) ? -SCORE_INFINITE : std::max(-SCORE_INFINITE, prev_score - delta);
        int asp_beta = (delta == SCORE_INFINITE) ? SCORE_INFINITE : std::min(SCORE_INFINITE, prev_score + delta);

        while (!worker_stop.load(std::memory_order_relaxed)) {
            int alpha = asp_alpha;
            int beta = asp_beta;
            int best_score = -SCORE_INFINITE;
            Move best_m = Move::none();
            std::vector<Move> best_pv;

            for (int i = 0; i < legal.count; ++i) {
                if (check_stop() || worker_stop.load(std::memory_order_relaxed)) return;
                const Move& m = legal.moves[i];
                int8_t mf = move_end_facing(pos, m);
                int m_end_sq = (m.kind == MoveKind::Rotate) ? m.src : m.dst;
                move_stack_[0] = m;
                move_f_stack_[0] = mf;
                move_ptsq_stack_[0] = (m_end_sq >= 0 && m_end_sq < NUM_SQUARES) ? static_cast<int16_t>(ptsq_idx(m.ptype, m_end_sq)) : int16_t(-1);
                move_pfsq_stack_[0] = (m_end_sq >= 0 && m_end_sq < NUM_SQUARES) ? static_cast<int16_t>(pfsq_idx(m.ptype, mf, m_end_sq)) : int16_t(-1);
                reduction_stack_[0] = 0;
                UndoState u;
                apply_move(pos, m, u);
                nnue_invalidate(1);
                ++nodes_;
                record_path_node(pos, 1);

                Side enemy = static_cast<Side>(pos.side_to_move);
                bool gives_chk = is_in_check(pos, enemy);
                int ext = (d == 1 && gives_chk) ? 1 : 0;

                int score = 0;
                if (count_repetitions(pos.zobrist, 1) >= 3) {
                    undo_move(pos, m, u);
                    pv_length_[1] = 1;
                    score = has_any_non_rep3_move(pos, 0) ? -(SCORE_MATE - 1) : 0;
                } else {
                    if (i == 0) {
                        score = -pvs(pos, d - 1 + ext, -beta, -alpha, 1, true, 4, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, false, 0);
                    } else {
                        int root_red = (d >= 3 && i >= 8 && !gives_chk && scores[i] < 180000) ? 1 : 0;
                        reduction_stack_[0] = static_cast<int8_t>(root_red);
                        score = -pvs(pos, std::max(0, d - 1 + ext - root_red), -(alpha + 1), -alpha, 1, false, 4, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, true, 0);
                        if (score > alpha && root_red > 0 && !abort_) {
                            reduction_stack_[0] = 0;
                            score = -pvs(pos, d - 1 + ext, -(alpha + 1), -alpha, 1, false, 4, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, true, 0);
                        }
                        if (score > alpha && score < beta && !abort_) {
                            reduction_stack_[0] = 0;
                            score = -pvs(pos, d - 1 + ext, -beta, -alpha, 1, true, 4, true, m, Move::none(), mf, 0, Move::none(), nullptr, gives_chk ? 1 : 0, -1, false, 0);
                        }
                    }
                    undo_move(pos, m, u);
                }
                if (abort_ || worker_stop.load(std::memory_order_relaxed)) return;

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
                        if (alpha >= beta) {
                            break;
                        }
                    }
                }
            }

            if (best_score <= asp_alpha && asp_alpha > -SCORE_INFINITE) {
                delta *= 2;
                if (delta > 400 || std::abs(best_score) >= 2000) {
                    asp_alpha = -SCORE_INFINITE;
                    asp_beta = SCORE_INFINITE;
                } else {
                    asp_alpha = std::max(-SCORE_INFINITE, prev_score - delta);
                }
                continue;
            }
            if (best_score >= asp_beta && asp_beta < SCORE_INFINITE) {
                delta *= 2;
                if (delta > 400 || std::abs(best_score) >= 2000) {
                    asp_alpha = -SCORE_INFINITE;
                    asp_beta = SCORE_INFINITE;
                } else {
                    asp_beta = std::min(SCORE_INFINITE, prev_score + delta);
                }
                continue;
            }

            if (!best_m.is_none()) {
                prev_score = best_score;
                voted_depth_ = d;
                voted_score_ = best_score;
                voted_move_ = best_m;
                voted_pv_ = std::move(best_pv);
                for (int ri = 0; ri < legal.count; ++ri) {
                    if (legal.moves[ri] == best_m) {
                        Move tmp = legal.moves[ri];
                        int tmp_sc = scores[ri];
                        for (int k = ri; k > 0; --k) {
                            legal.moves[k] = legal.moves[k - 1];
                            scores[k] = scores[k - 1];
                        }
                        legal.moves[0] = tmp;
                        scores[0] = tmp_sc;
                        break;
                    }
                }
            }
            break;
        }
        if (worker_stop.load(std::memory_order_relaxed)) return;
    }
}

int Searcher::extract_qleaf(const Position& start_pos, Position& out_leaf_pos, int& out_qply_len) {
    Position pos = start_pos;
    for (int s = 0; s < 2; ++s) {
        if (pos.king_sq[s] >= NUM_SQUARES) {
            Bitboard kb = pos.bb_side[s] & pos.bb_type[VUONG];
            if (kb.any()) pos.king_sq[s] = static_cast<uint8_t>(kb.lsb());
        }
    }

    limits_ = SearchLimits{};
    limits_.infinite = true;
    limits_.max_nodes = 50000000ULL;
    stop_flag_ = nullptr;
    abort_ = false;
    current_root_depth_ = 1;
    nodes_ = 0;
    start_time_ms_ = now_ms();
    hard_limit_ms_ = 1000000000LL;
    base_rep_.clear();
    mate_rep_.clear();
    rebuild_base_rep_bloom();
    std::fill_n(move_ptsq_stack_, MAX_PLY + 4, int16_t(-1));
    std::fill_n(move_pfsq_stack_, MAX_PLY + 4, int16_t(-1));
    std::memset(reduction_stack_, 0, sizeof(reduction_stack_));
    record_path_node(pos, 0);
    nnue_prepare_root();
    pv_length_[0] = 0;

    in_extract_qleaf_ = true;
    int qscore = qsearch(pos, -SCORE_INFINITE, SCORE_INFINITE, 0, 0);
    in_extract_qleaf_ = false;

    Position sim = pos;
    int applied = 0;
    for (int k = 0; k < pv_length_[0]; ++k) {
        const Move& pm = pv_table_[0][k];
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
        UndoState u;
        apply_move(sim, pm, u);
        ++applied;
    }

    out_leaf_pos = sim;
    out_qply_len = applied;
    return qscore;
}

void Searcher::collect_staged_moves(
    Position& pos,
    Move tt_move,
    Move killer1,
    Move killer2,
    Move counter_move,
    MoveList& out_moves
) {
    out_moves.clear();
    Side stm = static_cast<Side>(pos.side_to_move);
    bool in_chk = is_in_check(pos, stm);

    MoveList& legal = ply_stack_[0].moves;
    MoveList& caps_buf = ply_stack_[0].caps;
    MoveList& bad_caps_buf = ply_stack_[0].bad_caps;
    int* scores = ply_stack_[0].scores;
    int* bad_cap_scores = ply_stack_[0].bad_cap_scores;

    bool tt_yielded = false;
    Move yielded_specials[3]{};
    int num_yielded_specials = 0;

    // STAGE_TT
    if (!tt_move.is_none() && is_legal_move_fast(pos, tt_move, in_chk)) {
        tt_yielded = true;
        out_moves.add(tt_move);
    }

    // STAGE_CAPTURES
    generate_capture_moves(pos, caps_buf);
    legal.clear();
    bad_caps_buf.clear();
    for (int ci = 0; ci < caps_buf.count; ++ci) {
        const Move cm = caps_buf.moves[ci];
        if (tt_yielded && cm == tt_move) continue;
        if (!check_move_leaves_king_safe(pos, cm, in_chk)) continue;
        int v_dst = cell_material_value(pos, cm.dst);
        int v_att = (cm.kind == MoveKind::Team)
            ? cell_material_value(pos, cm.src)
            : piece_material_value(pos.top_piece[cm.src].type);
        int see_gain = stack_see_estimate(pos, cm);
        if (see_gain >= 0) {
            scores[legal.count] = 1000000 + see_gain * 64 + (10 * v_dst - v_att);
            legal.add(cm);
        } else if (bad_caps_buf.count < 256) {
            bad_cap_scores[bad_caps_buf.count] = -100000 + see_gain * 64 + (10 * v_dst - v_att);
            bad_caps_buf.add(cm);
        } else {
            scores[legal.count] = -100000 + see_gain * 64 + (10 * v_dst - v_att);
            legal.add(cm);
        }
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
        out_moves.add(legal.moves[i]);
    }

    // STAGE_KILLERS_COUNTER
    Move special_cands[3] = {killer1, killer2, counter_move};
    for (int si = 0; si < 3; ++si) {
        Move km = special_cands[si];
        if (km.is_none()) continue;
        if (tt_yielded && km == tt_move) continue;
        bool dup = false;
        for (int y = 0; y < num_yielded_specials; ++y) {
            if (yielded_specials[y] == km) { dup = true; break; }
        }
        if (dup) continue;
        if (!is_pseudo_legal_move(pos, km)) continue;
        if (is_capture_move(pos, km)) continue;
        if (!check_move_leaves_king_safe(pos, km, in_chk)) continue;
        yielded_specials[num_yielded_specials++] = km;
        out_moves.add(km);
    }

    // STAGE_QUIETS_AND_DROPS
    generate_quiet_and_drop_moves(pos, caps_buf);
    legal.clear();
    for (int qi = 0; qi < caps_buf.count; ++qi) {
        const Move qm = caps_buf.moves[qi];
        if (tt_yielded && qm == tt_move) continue;
        bool dup = false;
        for (int y = 0; y < num_yielded_specials; ++y) {
            if (yielded_specials[y] == qm) { dup = true; break; }
        }
        if (dup) continue;
        if (!check_move_leaves_king_safe(pos, qm, in_chk)) continue;
        legal.add(qm);
    }
    score_moves(pos, legal, scores, Move::none(), 1, counter_move, Move::none(), Move::none());
    for (int bi = 0; bi < bad_caps_buf.count; ++bi) {
        if (legal.count < MAX_MOVES) {
            scores[legal.count] = bad_cap_scores[bi];
            legal.add(bad_caps_buf.moves[bi]);
        }
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
        out_moves.add(legal.moves[i]);
    }
}

} // namespace homei

#pragma once

#include "mate.h"
#include "movegen.h"
#include "nnue/nnue.h"
#include "tt.h"
#include "types.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace homei {

struct SearchLimits {
    int id = 1;
    int max_depth = 32;
    uint64_t max_nodes = 50000000ULL;
    int64_t movetime_ms = 5000;
    int multipv = 1;
    bool infinite = false;
    int64_t camtime_ms = 0;
    int64_t xanhtime_ms = 0;
    int64_t caminc_ms = 0;
    int64_t xanhinc_ms = 0;
    int64_t camperiod_ms = 0;
    int64_t xanhperiod_ms = 0;
    int64_t optimum_time_ms = 0;
    int64_t maximum_time_ms = 0;
};

struct PVLine {
    int multipv_rank = 1;
    int depth = 1;
    int score = 0; // from root side_to_move perspective
    uint64_t nodes = 0;
    int64_t time_ms = 0;
    std::vector<Move> pv;
};

using InfoCallback = std::function<void(const PVLine&)>;

struct SearchResult {
    Move best_move = Move::none();
    std::vector<PVLine> lines;
    uint64_t total_nodes = 0;
    int64_t elapsed_ms = 0;
};

#ifdef HOMEI_STATS
struct SearchStats {
    uint64_t nodes = 0;
    uint64_t qnodes = 0;
    uint64_t beta_cutoffs_total = 0;
    uint64_t beta_cutoffs_first_move = 0;
    uint64_t tt_probe = 0;
    uint64_t tt_hit = 0;
    uint64_t tt_cutoff = 0;
    uint64_t lmr_researches = 0;
    uint64_t null_move_cutoffs = 0;

    void reset() {
        nodes = 0;
        qnodes = 0;
        beta_cutoffs_total = 0;
        beta_cutoffs_first_move = 0;
        tt_probe = 0;
        tt_hit = 0;
        tt_cutoff = 0;
        lmr_researches = 0;
        null_move_cutoffs = 0;
    }
};
#endif

class Searcher {
public:
    explicit Searcher(bool is_worker = false);
    ~Searcher();

    void clear_tt() {
        tt_.clear();
        for (auto& c : mate_cache_) {
            c.valid = false;
            c.pv.clear();
        }
        mate_cache_next_ = 0;
        clear_dfpn_tt();
        clear_dfpn_pending_ = true;
        last_search_ply_ = -1;
        for (auto& w : workers_) {
            w->clear_ordering_tables();
        }
    }
    void resize_tt(size_t mb) { tt_.resize(mb); }

    void set_threads(int threads) {
        num_threads_ = std::clamp(threads, 1, 32);
        ensure_workers();
    }
    int threads() const { return num_threads_; }
    void set_root_ply(int ply) { root_ply_ = ply; }

    void set_shared_tt(TranspositionTable* shared_tt) { tt_ptr_ = shared_tt; }
    struct BaseRepEntry {
        uint64_t hash;
        int count;
    };
    void set_reps(const std::vector<BaseRepEntry>& br, const std::vector<RepHashEntry>& mr) {
        base_rep_ = br;
        mate_rep_ = mr;
        rebuild_base_rep_bloom();
    }
    uint64_t nodes() const { return nodes_; }
    void search_worker(const Position& root_pos, const SearchLimits& limits, std::atomic<bool>& stop_flag, int thread_id);

    // Persistent ThreadPool worker controls
    void start_worker_loop(int thread_id);
    void stop_worker_loop();
    void dispatch_job(
        const Position& root_pos,
        const SearchLimits& limits,
        std::atomic<bool>& stop_flag,
        const std::vector<BaseRepEntry>& br,
        const std::vector<RepHashEntry>& mr,
        int root_ply,
        bool age_tables
    );
    void wait_job_done();

    static inline int piece_facing_idx(uint8_t ptype, int8_t facing) {
        return (static_cast<int>(ptype & 7) << 2) | (static_cast<int>(std::max<int8_t>(0, facing)) & 3);
    }

    static constexpr int CONT_PF_COUNT = 23;
    static constexpr int CONT_PT_SQ = 8 * NUM_SQUARES;              // 648
    static constexpr int CONT_PF_SQ = CONT_PF_COUNT * NUM_SQUARES;  // 1863

    static inline int pf23_idx(uint8_t ptype, int8_t facing) {
        constexpr uint8_t BASE[8] = {0, 1, 5, 9, 13, 17, 18, 19};
        uint8_t pt = ptype & 7;
        return static_cast<int>(BASE[pt]) + (can_rotate(pt) ? (static_cast<int>(std::max<int8_t>(0, facing)) & 3) : 0);
    }

    static inline int ptsq_idx(uint8_t ptype, int sq) {
        return static_cast<int>(ptype & 7) * NUM_SQUARES + sq;
    }

    static inline int pfsq_idx(uint8_t ptype, int8_t facing, int sq) {
        return pf23_idx(ptype, facing) * NUM_SQUARES + sq;
    }

    static inline int8_t move_end_facing(const Position& pos, const Move& m) {
        if (m.is_none()) return 0;
        if (m.kind == MoveKind::Drop) return static_cast<int8_t>(m.facing & 3);
        if (m.src >= 0 && m.src < NUM_SQUARES && pos.height[m.src] > 0) {
            return static_cast<int8_t>((pos.top_piece[m.src].facing + m.rotate_after) & 3);
        }
        return 0;
    }

#ifdef HOMEI_STATS
    const SearchStats& stats() const { return stats_; }
    void reset_stats() { stats_.reset(); }
#endif

    SearchResult search(
        const GameState& state,
        const SearchLimits& limits,
        std::atomic<bool>& stop_flag,
        const InfoCallback& on_info = nullptr
    );

    int extract_qleaf(const Position& start_pos, Position& out_leaf_pos, int& out_qply_len);
    void collect_staged_moves(Position& pos, Move tt_move, Move killer1, Move killer2, Move counter_move, MoveList& out_moves);

private:
#ifdef HOMEI_STATS
    SearchStats stats_;
#endif
    TranspositionTable tt_;
    TranspositionTable* tt_ptr_ = &tt_;
    int num_threads_ = 1;
    int thread_id_ = 0;
    bool is_worker_ = false;
    bool clear_dfpn_pending_ = false;
    std::vector<std::unique_ptr<Searcher>> workers_;

    // Persistent worker thread state
    std::thread worker_thread_;
    std::mutex worker_mtx_;
    std::condition_variable worker_cv_;
    std::condition_variable worker_done_cv_;
    bool worker_has_job_ = false;
    bool worker_busy_ = false;
    bool worker_exit_ = false;
    Position job_root_pos_{};
    SearchLimits job_limits_{};
    std::atomic<bool>* job_stop_ = nullptr;
    bool job_age_tables_ = false;
    int job_root_ply_ = 0;

    // Best-Thread Voting snapshot (updated by each worker after completing an iteration)
    int voted_depth_ = 0;
    int voted_score_ = 0;
    Move voted_move_ = Move::none();
    std::vector<Move> voted_pv_;

    // Search-local state
    SearchLimits limits_;
    std::atomic<bool>* stop_flag_ = nullptr;
    bool abort_ = false;
    int current_root_depth_ = 1;
    // True once at least one root move has been fully scored in this search (main thread).
    // Lets depth 1 honour movetime/stop instead of always running to completion.
    bool root_has_result_ = false;
    uint64_t nodes_ = 0;
    std::atomic<uint64_t> live_nodes_atomic_{0};
    int64_t start_time_ms_ = 0;
    int64_t hard_limit_ms_ = 0;

    // Initial repetition counts from GameState.history + O(1) 256-bit bloom filter
    std::vector<BaseRepEntry> base_rep_;
    std::vector<RepHashEntry> mate_rep_;
    uint64_t base_rep_bloom_[4]{};
    void rebuild_base_rep_bloom() {
        base_rep_bloom_[0] = base_rep_bloom_[1] = base_rep_bloom_[2] = base_rep_bloom_[3] = 0ULL;
        for (const auto& e : base_rep_) {
            uint64_t h = e.hash ^ (e.hash >> 29);
            base_rep_bloom_[(h >> 6) & 3] |= (1ULL << (h & 63));
        }
    }

    // Search stack (heap-allocated to prevent thread stack overflow)
    struct PlyStack {
        MoveList moves;
        MoveList caps;
        MoveList bad_caps;
        int scores[MAX_MOVES]{};
        int bad_cap_scores[MAX_MOVES]{};
        uint8_t fam_rank[MAX_MOVES]{};
    };
    std::vector<PlyStack> ply_stack_;

    uint64_t path_hashes_[MAX_PLY + 4]{};
    uint64_t path_board_keys_[MAX_PLY + 4]{};
    uint64_t path_hand_keys_[MAX_PLY + 4]{};
    uint64_t path_hand_packed_[MAX_PLY + 4][2]{};
    uint8_t  path_reserve_[MAX_PLY + 4][2]{};
    int16_t  eval_stack_[MAX_PLY + 4]{};
    int8_t   reduction_stack_[MAX_PLY + 4]{};
    int16_t  move_ptsq_stack_[MAX_PLY + 4]{};
    int16_t  move_pfsq_stack_[MAX_PLY + 4]{};
    Move     move_stack_[MAX_PLY + 4]{};
    int8_t   move_f_stack_[MAX_PLY + 4]{};

    Move killers_[MAX_PLY + 4][2]{};
    int history_[2][8][4][NUM_SQUARES]{};
    int drop_history_[2][8][4][NUM_SQUARES]{};
    int cap_history_[2][8][8][NUM_SQUARES]{};
    Move countermoves_[2][8][NUM_SQUARES]{};
    int last_search_ply_ = -1;
    int root_ply_ = 0;
    bool in_extract_qleaf_ = false;

    // 5-Table L2-resident Correction History (~96 KB per thread, strictly separate from rawEval in TT!)
    static constexpr int CORR_SIZE = 4096;
    static constexpr int CORR_MASK = CORR_SIZE - 1;
    int16_t corr_binh_[2][CORR_SIZE]{};
    int16_t corr_minor_[2][CORR_SIZE]{};
    int16_t corr_non_binh_[2][2][CORR_SIZE]{};
    int16_t corr_cont2_[2][CORR_SIZE]{};
    int16_t corr_cont46_[2][CORR_SIZE]{};

    // Thread-Local Facing-Aware Continuation History (1-ply pf->pf, 2-ply pt->pf, 4-ply pt->pt; zero atomic_ref!)
    std::vector<int16_t> cont_hist_1ply_; // [1863][1863]
    std::vector<int16_t> cont_hist_2ply_; // [648][1863]
    std::vector<int16_t> cont_hist_4ply_; // [648][648]

    // Thread-local lazy multi-ply NNUE accumulator stack + Finny table
    std::shared_ptr<const nnue::Network> active_nnue_;
    std::vector<nnue::PlyAccNode> nnue_stack_;
    std::unique_ptr<nnue::FinnyTable> nnue_finny_;
    uint64_t nnue_finny_net_id_ = 0;

    inline void nnue_prepare_root() {
        active_nnue_ = nnue::is_nnue_enabled() ? nnue::get_global_network() : nullptr;
        if (active_nnue_) {
            if (!nnue_finny_) {
                nnue_finny_ = std::make_unique<nnue::FinnyTable>();
                nnue_finny_net_id_ = 0;
            }
            if (nnue_finny_net_id_ != active_nnue_->id) {
                nnue_finny_->clear();
                nnue_finny_net_id_ = active_nnue_->id;
            }
            nnue_stack_[0].acc.computed[0] = false;
            nnue_stack_[0].acc.computed[1] = false;
        }
    }

    inline void nnue_invalidate(int ply) {
        if (active_nnue_) {
            nnue_stack_[ply].acc.computed[0] = false;
            nnue_stack_[ply].acc.computed[1] = false;
        }
    }

    int eval_node(const Position& pos, int ply, int alpha, int beta, bool allow_lazy, bool& out_is_lazy);

    Move pv_table_[MAX_PLY + 4][MAX_PLY + 4]{};
    int pv_length_[MAX_PLY + 4]{};

    struct CachedMateNode {
        uint64_t hash = 0;
        bool valid = false;
        bool is_attacker = false; // true = +mate_ply, false = -mate_ply
        int mate_ply = 0;
        Move best_move = Move::none();
        std::vector<Move> pv;
    };
    static constexpr size_t MATE_CACHE_SIZE = 256;
    CachedMateNode mate_cache_[MATE_CACHE_SIZE]{};
    size_t mate_cache_next_ = 0;

    void cache_mate_pv(const Position& start_pos, const std::vector<Move>& pv, bool root_is_attacker);
    const CachedMateNode* lookup_mate_cache(uint64_t hash) const;

    int correction_delta(const Position& pos, int ply) const;
    void update_correction_history(const Position& pos, int ply, int depth, int diff);
    int check_superior_inferior_hand(const Position& pos, int ply) const;
    void record_path_node(const Position& pos, int ply);
    int get_5ply_cont_hist(int ply, uint8_t pt, int8_t end_f, int end_sq) const;
    void update_5ply_cont_hist(int ply, uint8_t pt, int8_t end_f, int end_sq, int bonus);

    bool check_stop();
    int count_repetitions(uint64_t hash, int ply) const;
    bool has_any_non_rep3_move(Position& pos, int ply) const;
    int qsearch(Position& pos, int alpha, int beta, int ply, int qply, bool* out_rep_tainted = nullptr,
                int8_t known_in_check = -1, int known_rep = -1);
    int pvs(
        Position& pos, int depth, int alpha, int beta, int ply, bool is_pv, int ext_budget,
        bool allow_nmp = true, Move prev_move = Move::none(), Move prev2_move = Move::none(),
        int8_t prev1_f = 0, int8_t prev2_f = 0, Move excluded_move = Move::none(),
        bool* out_rep_tainted = nullptr, int8_t known_in_check = -1, int known_rep = -1,
        bool cut_node = false, int double_ext = 0
    );
    void score_moves(
        Position& pos, const MoveList& list, int* scores, Move tt_move, int ply,
        Move counter_move = Move::none(), Move prev1_move = Move::none(), Move prev2_move = Move::none(),
        int8_t prev1_f = 0, int8_t prev2_f = 0, bool quiets_only = false, uint8_t* out_fam_rank = nullptr
    ) const;
    void clear_ordering_tables();
    void age_ordering_tables();
    void ensure_workers();
};

} // namespace homei

#pragma once

#include "mate.h"
#include "movegen.h"
#include "tt.h"
#include "types.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
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
    Searcher();
    ~Searcher();

    void clear_tt() { tt_.clear(); }
    void resize_tt(size_t mb) { tt_.resize(mb); }

    void set_threads(int threads) { num_threads_ = std::clamp(threads, 1, 32); }
    int threads() const { return num_threads_; }

    void set_shared_tt(TranspositionTable* shared_tt) { tt_ptr_ = shared_tt; }
    void set_shared_cont_hist(int16_t (*c1)[NUM_SQUARES][32][NUM_SQUARES], int16_t (*c2)[NUM_SQUARES][32][NUM_SQUARES]) {
        cont_hist_1ply_ = c1;
        cont_hist_2ply_ = c2;
        owns_cont_hist_ = false;
    }
    uint64_t nodes() const { return nodes_; }
    void search_worker(const Position& root_pos, const SearchLimits& limits, std::atomic<bool>& stop_flag, int thread_id);

    static inline int piece_facing_idx(uint8_t ptype, int8_t facing) {
        return (static_cast<int>(ptype & 7) << 2) | (static_cast<int>(std::max<int8_t>(0, facing)) & 3);
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

private:
#ifdef HOMEI_STATS
    SearchStats stats_;
#endif
    TranspositionTable tt_;
    TranspositionTable* tt_ptr_ = &tt_;
    int num_threads_ = 1;
    bool owns_cont_hist_ = true;

    // Search-local state
    SearchLimits limits_;
    std::atomic<bool>* stop_flag_ = nullptr;
    bool abort_ = false;
    int current_root_depth_ = 1;
    uint64_t nodes_ = 0;
    int64_t start_time_ms_ = 0;
    int64_t hard_limit_ms_ = 0;

    // Initial repetition counts from GameState.history
    struct BaseRepEntry {
        uint64_t hash;
        int count;
    };
    std::vector<BaseRepEntry> base_rep_;
    std::vector<RepHashEntry> mate_rep_;

    // Search stack (heap-allocated to prevent thread stack overflow)
    struct PlyStack {
        MoveList moves;
        MoveList caps;
        int scores[MAX_MOVES]{};
    };
    std::vector<PlyStack> ply_stack_;

    uint64_t path_hashes_[MAX_PLY + 4]{};
    Move killers_[MAX_PLY + 4][2]{};
    int history_[2][8][NUM_SQUARES]{};
    Move countermoves_[2][8][NUM_SQUARES]{};
    Move move_history_[MAX_PLY + 4]{};

    // Continuation History: [prev_pf 32][prev_to 81][curr_pf 32][curr_to 81]
    int16_t (*cont_hist_1ply_)[NUM_SQUARES][32][NUM_SQUARES] = nullptr;
    int16_t (*cont_hist_2ply_)[NUM_SQUARES][32][NUM_SQUARES] = nullptr;

    Move pv_table_[MAX_PLY + 4][MAX_PLY + 4]{};
    int pv_length_[MAX_PLY + 4]{};

    bool check_stop();
    int count_repetitions(uint64_t hash, int ply) const;
    int qsearch(Position& pos, int alpha, int beta, int ply, int qply);
    int pvs(Position& pos, int depth, int alpha, int beta, int ply, bool is_pv, int ext_budget, bool allow_nmp = true, Move prev_move = Move::none(), Move prev2_move = Move::none());
    void score_moves(Position& pos, const MoveList& list, int* scores, Move tt_move, int ply, Move counter_move = Move::none(), Move prev1_move = Move::none(), Move prev2_move = Move::none()) const;
};

} // namespace homei

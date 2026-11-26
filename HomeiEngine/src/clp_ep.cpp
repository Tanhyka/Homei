#include "adjudicate.h"
#include "attack.h"
#include "base64url.h"
#include "book.h"
#include "eval.h"
#include "features.h"
#include "geometry.h"
#include "json.h"
#include "lppn.h"
#include "movegen.h"
#include "nnue/nnue.h"
#include "nnue/train_data.h"
#include "notation.h"
#include "search.h"
#include "see.h"
#include "state_json.h"
#include "tt.h"
#include "types.h"
#include "zobrist.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace homei {

static std::string get_engine_name() {
#ifdef _WIN32
    char path_buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path_buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::error_code ec;
        std::filesystem::path p(path_buf);
        std::string stem = p.stem().string();
        if (!stem.empty()) {
            if (stem == "homei" || stem == "clp_core_cli") {
                return "Homei";
            }
            if (stem.rfind("Homei_", 0) == 0) {
                std::string pretty = stem;
                for (char& c : pretty) {
                    if (c == '_') c = ' ';
                }
                return pretty;
            }
            return stem;
        }
    }
#endif
    return "Homei";
}

class EngineApp {
public:
    EngineApp() {
        init_zobrist();
        init_attack_tables();
        Position init_pos;
        setup_initial_position(init_pos);
        current_state_ = make_base_state(init_pos, 0);
        has_state_ = true;
        if (const char* env_t = std::getenv("HOMEI_THREADS")) {
            int t = std::atoi(env_t);
            if (t >= 1 && t <= 32) {
                opt_threads_ = t;
                searcher_.set_threads(t);
            }
        }
        auto_load_default_nnue();
        search_thread_ = std::thread(&EngineApp::search_worker_loop, this);
    }

    ~EngineApp() {
        stop_active_search();
        {
            std::lock_guard<std::mutex> lk(worker_mutex_);
            worker_exit_ = true;
        }
        worker_cv_.notify_all();
        if (search_thread_.joinable()) {
            search_thread_.join();
        }
    }

    void run() {
        std::ios::sync_with_stdio(false);
        std::cin.tie(nullptr);

        std::string line;
        line.reserve(4096);
        constexpr size_t MAX_LINE_LEN = 2 * 1024 * 1024; // 2 MiB

        while (true) {
            line.clear();
            int ch = 0;
            bool overflow = false;
            bool got_any = false;
            while ((ch = std::cin.get()) != EOF) {
                got_any = true;
                if (ch == '\n') break;
                if (ch == '\r') continue;
                if (line.size() < MAX_LINE_LEN) {
                    line.push_back(static_cast<char>(ch));
                } else {
                    overflow = true;
                }
            }
            if (!got_any && ch == EOF) {
                break;
            }
            if (overflow) {
                send_error("Line exceeds 2 MiB limit");
                continue;
            }

            // Trim leading/trailing whitespace
            size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos) continue;
            size_t last = line.find_last_not_of(" \t");
            std::string cmd = line.substr(first, last - first + 1);

            if (!handle_command(cmd)) {
                break;
            }
        }

        stop_active_search();
    }

private:
    std::mutex out_mutex_;
    GameState current_state_;
    bool has_state_ = false;
    Searcher searcher_;

    // Options
    int opt_depth_ = 32;
    uint64_t opt_nodes_ = 50000000ULL;
    int opt_multipv_ = 1;
    int opt_threads_ = 1;
    size_t opt_hash_mb_ = 64; // must match Searcher's default TT size
    bool opt_own_book_ = EngineFeatures::get().own_book;
    int opt_book_depth_ = 18;
    bool opt_book_random_ = true;
    std::string opt_book_file_;
    book::OpeningBook opening_book_;
    bool opt_use_nnue_ = true;
    std::string opt_nnue_file_;

    void auto_load_default_nnue() {
        std::vector<std::filesystem::path> cands;
        if (const char* env_nn = std::getenv("HOMEI_NNUE_FILE")) {
            if (env_nn[0] != '\0') {
                cands.emplace_back(env_nn);
            }
        }
#ifdef _WIN32
        char path_buf[MAX_PATH];
        DWORD len = GetModuleFileNameA(NULL, path_buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::filesystem::path exe_dir = std::filesystem::path(path_buf).parent_path();
            cands.push_back(exe_dir / "homei_v46_gen1.nn");
            cands.push_back(exe_dir / "homei.nn");
            cands.push_back(exe_dir.parent_path() / "networks" / "homei_v46_gen1.nn");
        }
#endif
        std::error_code ec;
        for (const auto& p : cands) {
            if (std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec)) {
                std::string err;
                if (nnue::load_global_network(p.string(), err)) {
                    opt_nnue_file_ = p.string();
                    return;
                }
            }
        }
    }

    // Active background search (persistent worker thread reuses df-pn TT & avoids per-move thread churn)
    std::atomic<bool> stop_flag_{false};
    std::atomic<int> active_id_{0};
    std::mutex cv_mutex_;
    std::condition_variable cv_stop_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::condition_variable worker_done_cv_;
    bool worker_has_job_ = false;
    bool worker_busy_ = false;
    bool worker_exit_ = false;
    GameState worker_state_;
    SearchLimits worker_limits_;
    std::thread search_thread_;

    void send_line(const std::string& s) {
        std::lock_guard<std::mutex> lock(out_mutex_);
        std::cout << s << '\n' << std::flush;
    }

    void send_error(const std::string& reason) {
        std::string clean = reason;
        for (char& c : clean) {
            if (c == '\n' || c == '\r') c = ' ';
        }
        if (clean.size() > 280) {
            clean.resize(280);
        }
        send_line("error " + clean);
    }

    void stop_active_search() {
        {
            std::lock_guard<std::mutex> lk(cv_mutex_);
            stop_flag_.store(true, std::memory_order_relaxed);
        }
        cv_stop_.notify_all();
        {
            std::unique_lock<std::mutex> lk(worker_mutex_);
            while (worker_busy_) {
                if (!worker_done_cv_.wait_for(lk, std::chrono::milliseconds(100), [this]() { return !worker_busy_; })) {
                    lk.unlock();
                    stop_flag_.store(true, std::memory_order_relaxed);
                    cv_stop_.notify_all();
                    lk.lock();
                }
            }
        }
        active_id_.store(0, std::memory_order_relaxed);
    }

    bool handle_command(const std::string& cmd) {
        if (cmd == "quit") {
            stop_active_search();
            return false;
        }
        if (cmd == "clp 1") {
            std::string eng_name = get_engine_name();
            send_line("id name " + eng_name);
            send_line("id author Homei Team");
            send_line("protocol clp-ep 1");
            send_line("capability search legal");
            send_line("capability clocks movetime");
            send_line("capability search infinite");
            send_line("capability control stop");
            send_line("option name Depth type spin default 32 min 1 max 32");
            send_line("option name Nodes type spin default 50000000 min 1 max 50000000");
            send_line("option name MultiPV type spin default 1 min 1 max 5");
            send_line("option name Threads type spin default 1 min 1 max 32");
            send_line("option name Hash type spin default 64 min 1 max 2048");
            send_line("option name OwnBook type spin default 1 min 0 max 1");
            send_line("option name BookDepth type spin default 18 min 0 max 32");
            send_line("option name BookRandom type spin default 1 min 0 max 1");
            send_line("option name BookFile type string default ");
            send_line("option name UseNNUE type spin default 1 min 0 max 1");
            send_line("option name NNUEFile type string default ");
            send_line("clpok");
            return true;
        }
        if (cmd == "isready") {
            send_line("readyok");
            return true;
        }
        if (cmd.rfind("stop", 0) == 0) {
            std::istringstream iss(cmd);
            std::string tok_stop, tok_id;
            int req_id = -1;
            if (iss >> tok_stop >> tok_id >> req_id && tok_stop == "stop" && tok_id == "id") {
                if (req_id > 0 && req_id == active_id_.load(std::memory_order_relaxed)) {
                    stop_active_search();
                }
            }
            return true;
        }
        if (cmd.rfind("position ", 0) == 0) {
            stop_active_search();
            std::string rest = cmd.substr(9);
            size_t p = rest.find_first_not_of(" \t");
            if (p == std::string::npos) {
                send_error("Missing position arguments");
                return true;
            }
            rest = rest.substr(p);
            if (rest.rfind("state64 ", 0) == 0) {
                std::string b64 = rest.substr(8);
                size_t bp = b64.find_first_not_of(" \t");
                if (bp == std::string::npos) {
                    send_error("Empty state64 token");
                    return true;
                }
                b64 = b64.substr(bp);
                std::string json_str;
                if (!base64url_decode(b64, json_str)) {
                    send_error("Invalid base64url in state64");
                    return true;
                }
                if (json_str.size() > 1048576ULL) {
                    send_error("State JSON exceeds 1 MiB limit");
                    return true;
                }
                GameState st;
                std::string err;
                if (!parse_state_json_str(json_str, st, err)) {
                    send_error(err);
                    return true;
                }
                if (st.result.status == GameStatus::Ongoing) {
                    std::string adj_err;
                    if (!adjudicate_state(st, adj_err)) {
                        send_error(adj_err);
                        return true;
                    }
                }
                current_state_ = std::move(st);
                has_state_ = true;
                send_line("positionok ply " + std::to_string(current_state_.ply));
                return true;
            }
            if (rest.rfind("lppn ", 0) == 0) {
                std::string lppn_str = rest.substr(5);
                Position pos;
                int ply = 0;
                std::string err;
                if (!parse_lppn(lppn_str, pos, ply, err, true)) {
                    send_error(err);
                    return true;
                }
                GameState st = make_base_state(pos, ply);
                if (!adjudicate_state(st, err)) {
                    send_error(err);
                    return true;
                }
                current_state_ = std::move(st);
                has_state_ = true;
                send_line("positionok ply " + std::to_string(current_state_.ply));
                return true;
            }
            if (rest == "startpos") {
                Position pos;
                setup_initial_position(pos);
                current_state_ = make_base_state(pos, 0);
                has_state_ = true;
                send_line("positionok ply 0");
                return true;
            }
            send_error("Unsupported position command");
            return true;
        }
        if (cmd.rfind("setoption ", 0) == 0) {
            stop_active_search();
            std::string rest = cmd.substr(10);
            size_t p_name = rest.find("name ");
            if (p_name != 0) {
                send_error("Invalid setoption syntax");
                return true;
            }
            rest = rest.substr(5);
            size_t p_val = rest.find(" value");
            if (p_val == std::string::npos) {
                send_error("Invalid setoption syntax");
                return true;
            }
            std::string name = rest.substr(0, p_val);
            std::string val_str;
            if (p_val + 6 < rest.size() && rest[p_val + 6] == ' ') {
                val_str = rest.substr(p_val + 7);
            } else if (p_val + 6 == rest.size()) {
                val_str = "";
            } else {
                send_error("Invalid setoption syntax");
                return true;
            }

            if (name == "BookFile") {
                std::string berr;
                if (!opening_book_.load_file(val_str, berr)) {
                    send_error(berr);
                    return true;
                }
                opt_book_file_ = val_str;
                send_line("optionok name BookFile value " + opt_book_file_);
                return true;
            }
            if (name == "NNUEFile") {
                std::string nerr;
                if (!nnue::load_global_network(val_str, nerr)) {
                    send_error(nerr);
                    return true;
                }
                opt_nnue_file_ = val_str;
                searcher_.clear_tt();
                send_line("optionok name NNUEFile value " + opt_nnue_file_);
                return true;
            }

            std::istringstream iss(val_str);
            int64_t val = 0;
            std::string extra;
            if (!(iss >> val) || (iss >> extra)) {
                send_error("Invalid setoption value");
                return true;
            }
            if (name == "Depth") {
                if (val < 1 || val > 32) {
                    send_error("Depth out of range 1..32");
                    return true;
                }
                opt_depth_ = static_cast<int>(val);
                send_line("optionok name Depth value " + std::to_string(opt_depth_));
                return true;
            }
            if (name == "Nodes") {
                if (val < 1 || val > 50000000LL) {
                    send_error("Nodes out of range 1..50000000");
                    return true;
                }
                opt_nodes_ = static_cast<uint64_t>(val);
                send_line("optionok name Nodes value " + std::to_string(opt_nodes_));
                return true;
            }
            if (name == "MultiPV") {
                if (val < 1 || val > 5) {
                    send_error("MultiPV out of range 1..5");
                    return true;
                }
                opt_multipv_ = static_cast<int>(val);
                send_line("optionok name MultiPV value " + std::to_string(opt_multipv_));
                return true;
            }
            if (name == "Threads") {
                if (val < 1 || val > 32) {
                    send_error("Threads out of range 1..32");
                    return true;
                }
                opt_threads_ = static_cast<int>(val);
                searcher_.set_threads(opt_threads_);
                send_line("optionok name Threads value " + std::to_string(opt_threads_));
                return true;
            }
            if (name == "Hash") {
                if (val < 1 || val > 2048) {
                    send_error("Hash out of range 1..2048");
                    return true;
                }
                opt_hash_mb_ = static_cast<size_t>(val);
                searcher_.resize_tt(opt_hash_mb_);
                send_line("optionok name Hash value " + std::to_string(val));
                return true;
            }
            if (name == "OwnBook") {
                if (val < 0 || val > 1) {
                    send_error("OwnBook out of range 0..1");
                    return true;
                }
                opt_own_book_ = (val != 0);
                send_line("optionok name OwnBook value " + std::to_string(opt_own_book_ ? 1 : 0));
                return true;
            }
            if (name == "BookDepth") {
                if (val < 0 || val > 32) {
                    send_error("BookDepth out of range 0..32");
                    return true;
                }
                opt_book_depth_ = static_cast<int>(val);
                send_line("optionok name BookDepth value " + std::to_string(opt_book_depth_));
                return true;
            }
            if (name == "BookRandom") {
                if (val < 0 || val > 1) {
                    send_error("BookRandom out of range 0..1");
                    return true;
                }
                opt_book_random_ = (val != 0);
                send_line("optionok name BookRandom value " + std::to_string(opt_book_random_ ? 1 : 0));
                return true;
            }
            if (name == "UseNNUE") {
                if (val < 0 || val > 1) {
                    send_error("UseNNUE out of range 0..1");
                    return true;
                }
                opt_use_nnue_ = (val != 0);
                nnue::set_nnue_enabled(opt_use_nnue_);
                searcher_.clear_tt();
                send_line("optionok name UseNNUE value " + std::to_string(opt_use_nnue_ ? 1 : 0));
                return true;
            }
            send_error("Unknown option name");
            return true;
        }
        if (cmd.rfind("go ", 0) == 0) {
            stop_active_search();
            if (!has_state_) {
                send_error("No position set before go");
                return true;
            }
            std::istringstream iss(cmd);
            std::string tok_go, tok_id, mode;
            int req_id = 0;
            if (!(iss >> tok_go >> tok_id >> req_id >> mode) || tok_go != "go" || tok_id != "id" || req_id <= 0) {
                send_error("Invalid go syntax");
                return true;
            }

            SearchLimits limits;
            limits.id = req_id;
            limits.multipv = opt_multipv_;

            if (mode == "infinite") {
                std::string extra;
                if (iss >> extra) {
                    send_error("Unexpected tokens after go infinite");
                    return true;
                }
                limits.infinite = true;
                limits.max_depth = 32;
                limits.max_nodes = UINT64_MAX;
                limits.movetime_ms = 86400000LL;
            } else if (mode == "movetime") {
                int64_t ms = 0;
                std::string extra;
                if (!(iss >> ms) || ms < 1 || ms > 86400000LL || (iss >> extra)) {
                    send_error("Invalid movetime value");
                    return true;
                }
                limits.infinite = false;
                limits.movetime_ms = ms;
                limits.max_depth = opt_depth_;
                limits.max_nodes = opt_nodes_;
            } else if (mode == "camtime" || mode == "xanhtime" || mode == "btime" || mode == "wtime" || mode == "clock" || mode == "camperiod" || mode == "xanhperiod" || mode == "byoyomi") {
                limits.infinite = false;
                limits.max_depth = opt_depth_;
                limits.max_nodes = opt_nodes_;
                std::string key = mode;
                int64_t val = 0;
                do {
                    if (key == "camtime" || key == "btime") {
                        if (iss >> val) limits.camtime_ms = std::max<int64_t>(0, val);
                    } else if (key == "xanhtime" || key == "wtime") {
                        if (iss >> val) limits.xanhtime_ms = std::max<int64_t>(0, val);
                    } else if (key == "caminc" || key == "binc") {
                        if (iss >> val) limits.caminc_ms = std::max<int64_t>(0, val);
                    } else if (key == "xanhinc" || key == "winc") {
                        if (iss >> val) limits.xanhinc_ms = std::max<int64_t>(0, val);
                    } else if (key == "camperiod") {
                        if (iss >> val) limits.camperiod_ms = std::max<int64_t>(0, val);
                    } else if (key == "xanhperiod") {
                        if (iss >> val) limits.xanhperiod_ms = std::max<int64_t>(0, val);
                    } else if (key == "byoyomi") {
                        if (iss >> val) {
                            int64_t byo = std::max<int64_t>(0, val);
                            limits.camperiod_ms = byo;
                            limits.xanhperiod_ms = byo;
                        }
                    }
                } while (iss >> key);

                Side stm = static_cast<Side>(current_state_.position.side_to_move);
                int64_t time_left = (stm == CAM) ? limits.camtime_ms : limits.xanhtime_ms;
                int64_t inc = (stm == CAM) ? limits.caminc_ms : limits.xanhinc_ms;
                int64_t byoyomi = (stm == CAM) ? limits.camperiod_ms : limits.xanhperiod_ms;

                // Phase-aware moves-to-go divisor: conservative in opening (~32), tighter in endgame (~16)
                int64_t mtg = std::clamp<int64_t>(32 - current_state_.ply / 6, 16, 32);
                int64_t opt_t = 0;
                int64_t max_t = 0;

                if (time_left > 0) {
                    if (byoyomi > 0) {
                        // Safety net exists in byoyomi -> spend main time slightly more proactively
                        int64_t byo_safe = std::max<int64_t>(20, byoyomi - std::clamp<int64_t>(byoyomi / 15, 60, 250));
                        int64_t adj_mtg = std::max<int64_t>(14, mtg - 4);
                        opt_t = std::max<int64_t>(byo_safe * 7 / 10, time_left / adj_mtg + inc * 3 / 4 + byo_safe / 3);
                        int64_t cap = (time_left > 2000) ? (time_left / 2 + byo_safe) : (time_left + byo_safe);
                        max_t = std::min<int64_t>(cap, std::max<int64_t>(opt_t * 3, byo_safe));
                    } else {
                        // Sudden death / Fischer increment: reserve IPC safety margin
                        int64_t reserve = std::clamp<int64_t>(time_left / 20, 40, 250);
                        int64_t usable = std::max<int64_t>(15, time_left - reserve);
                        opt_t = std::max<int64_t>(10, usable / mtg + inc * 3 / 4);
                        max_t = std::max<int64_t>(opt_t, std::min<int64_t>(opt_t * 3, usable / 3));
                    }
                } else if (byoyomi > 0) {
                    // Already in byoyomi: stay safely inside per-move countdown period
                    int64_t margin = std::clamp<int64_t>(byoyomi / 15, 60, 250);
                    int64_t usable_byo = std::max<int64_t>(25, byoyomi - margin);
                    opt_t = std::max<int64_t>(20, (usable_byo * 80) / 100);
                    max_t = usable_byo;
                } else {
                    opt_t = 200;
                    max_t = 600;
                }

                limits.optimum_time_ms = std::max<int64_t>(10, std::min(opt_t, max_t));
                limits.maximum_time_ms = std::max<int64_t>(limits.optimum_time_ms, max_t);
                limits.movetime_ms = limits.maximum_time_ms;
            } else {
                send_error("Unsupported go mode");
                return true;
            }

            start_search(current_state_, limits);
            return true;
        }

        // --- Test & Diagnostic Extension Commands (Section 3.3 line 162) ---
        if (cmd == "legal") {
            stop_active_search();
            MoveList legal;
            generate_legal_moves(current_state_.position, legal);
            std::vector<std::string> names;
            names.reserve(legal.count);
            for (int i = 0; i < legal.count; ++i) {
                names.push_back(format_canonical_notation(current_state_.position, legal.moves[i]));
            }
            std::sort(names.begin(), names.end());
            std::ostringstream oss;
            oss << "legal " << names.size();
            for (const auto& n : names) {
                oss << ' ' << n;
            }
            send_line(oss.str());
            return true;
        }
        if (cmd == "legal64") {
            stop_active_search();
            MoveList legal;
            generate_legal_moves(current_state_.position, legal);
            JsonValue arr = JsonValue::make_array();
            arr.arr_val.reserve(legal.count);
            for (int i = 0; i < legal.count; ++i) {
                std::string not_str = format_canonical_notation(current_state_.position, legal.moves[i]);
                arr.arr_val.push_back(encode_move_json(legal.moves[i], not_str));
            }
            std::string b64 = base64url_encode(json_stringify(arr));
            send_line("legal64 " + std::to_string(legal.count) + " " + b64);
            return true;
        }
        if (cmd.rfind("perft ", 0) == 0) {
            stop_active_search();
            std::istringstream iss(cmd);
            std::string tok;
            int d = -1;
            if (!(iss >> tok >> d) || d < 0 || d > 10) {
                send_error("Invalid perft depth");
                return true;
            }
            uint64_t count = perft(current_state_.position, d);
            send_line("perft " + std::to_string(d) + " " + std::to_string(count));
            return true;
        }
        if (cmd == "lppn") {
            stop_active_search();
            std::string lppn_out, err;
            if (!format_lppn(current_state_.position, current_state_.ply, lppn_out, err, false)) {
                send_error(err);
                return true;
            }
            send_line("lppn " + lppn_out);
            return true;
        }
        if (cmd.rfind("apply ", 0) == 0) {
            stop_active_search();
            std::string not_str = cmd.substr(6);
            size_t p = not_str.find_first_not_of(" \t");
            if (p == std::string::npos) {
                send_error("illegal move");
                return true;
            }
            not_str = not_str.substr(p);
            Move mv;
            std::string err;
            if (!parse_canonical_notation(current_state_.position, not_str, mv, err)) {
                send_error("illegal move");
                return true;
            }
            if (!apply_move_to_state(current_state_, mv, err)) {
                send_error("illegal move");
                return true;
            }
            send_line("positionok ply " + std::to_string(current_state_.ply));
            return true;
        }
        if (cmd == "state64") {
            stop_active_search();
            std::string json_s = json_stringify(encode_state_json(current_state_));
            send_line("state64 " + base64url_encode(json_s));
            return true;
        }
        if (cmd == "hash") {
            stop_active_search();
            send_line("hash " + std::to_string(current_state_.position.zobrist));
            return true;
        }
        if (cmd == "bookprobe") {
            stop_active_search();
            Move bmv = Move::none();
            int bsc = 0;
            book::ProbeOptions bopts;
            bopts.random_pick = false;
            if (opening_book_.probe(current_state_, bmv, bsc, bopts)) {
                std::string bnot = format_canonical_notation(current_state_.position, bmv);
                send_line("bookprobe hit " + bnot + " " + std::to_string(bsc) + " entries=" + std::to_string(opening_book_.entry_count()));
            } else {
                send_line("bookprobe miss entries=" + std::to_string(opening_book_.entry_count()));
            }
            return true;
        }
        if (cmd == "eval") {
            stop_active_search();
            const Position& pos = current_state_.position;
            Position mir = mirror_horizontal(pos);
            Position flp = flip_colors_180(pos);
            int ev = evaluate(pos);
            int ev_mir = evaluate(mir);
            int ev_cam = evaluate_for_side(pos, CAM);
            int ev_flp_cam = evaluate_for_side(flp, CAM);
            std::ostringstream oss;
            oss << "eval " << ev << " " << ev_mir << " " << ev_cam << " " << ev_flp_cam;
            send_line(oss.str());
            return true;
        }
        if (cmd == "nnueeval") {
            stop_active_search();
            auto net = nnue::get_global_network();
            if (!net || !net->loaded) {
                send_line("nnueeval none");
                return true;
            }
            const Position& pos = current_state_.position;
            Position mir = mirror_horizontal(pos);
            Position flp = flip_colors_180(pos);
            int ev = nnue::evaluate_position(*net, pos);
            int ev_mir = nnue::evaluate_position(*net, mir);
            int ev_flp = nnue::evaluate_position(*net, flp);
            int stk = nnue::select_layer_stack(pos);
            std::ostringstream oss;
            oss << "nnueeval " << ev << " " << ev_mir << " " << ev_flp << " stack=" << stk
                << " enabled=" << (nnue::is_nnue_enabled() ? 1 : 0)
                << " file=" << net->path;
            send_line(oss.str());
            return true;
        }
        if (cmd == "nnuetest") {
            stop_active_search();
            nnue::Network net;
            net.init_random_for_test(0x9E3779B97F4A7C15ULL);
            nnue::FinnyTable finny;
            finny.clear();
            uint64_t rng = 0xD1B54A32D192ED03ULL;
            auto next_rng = [&rng]() -> uint64_t {
                rng ^= rng >> 12;
                rng ^= rng << 25;
                rng ^= rng >> 27;
                return rng * 0x2545F4914F6CDD1DULL;
            };

            int total_plies = 0;
            int total_games = 8;
            for (int g = 0; g < total_games; ++g) {
                Position pos;
                setup_initial_position(pos);
                std::vector<nnue::PlyAccNode> stack(128);
                nnue::refresh_perspective(net, pos, CAM, stack[0], &finny);
                nnue::refresh_perspective(net, pos, XANH, stack[0], &finny);

                for (int ply = 0; ply < 80; ++ply) {
                    MoveList legal;
                    generate_legal_moves(pos, legal);
                    if (legal.count == 0) break;
                    Move mv = legal.moves[next_rng() % static_cast<uint64_t>(legal.count)];
                    UndoState u;
                    apply_move(pos, mv, u);
                    ++total_plies;

                    int next_p = ply + 1;
                    // Test BOTH 1-ply incremental AND lazy multi-ply skip (skip every 3rd ply!)
                    bool skip_ply = ((next_p % 3) == 1);
                    if (skip_ply) {
                        stack[next_p].acc.computed[0] = false;
                        stack[next_p].acc.computed[1] = false;
                        continue;
                    }
                    for (int p = 0; p < 2; ++p) {
                        int anc = next_p - 1;
                        while (anc >= 0 && !stack[anc].acc.computed[p]) --anc;
                        nnue::update_perspective(net, pos, p, stack[anc], stack[next_p], &finny);
                    }
                    nnue::PlyAccNode ref_node;
                    nnue::refresh_perspective(net, pos, CAM, ref_node, nullptr);
                    nnue::refresh_perspective(net, pos, XANH, ref_node, nullptr);

                    int ev_inc = nnue::evaluate_accumulator(net, pos, stack[next_p].acc);
                    int ev_ref = nnue::evaluate_accumulator(net, pos, ref_node.acc);
                    if (ev_inc != ev_ref ||
                        std::memcmp(stack[next_p].acc.v, ref_node.acc.v, sizeof(ref_node.acc.v)) != 0) {
                        send_line("nnuetest fail inc_mismatch game=" + std::to_string(g) + " ply=" + std::to_string(next_p)
                                  + " inc=" + std::to_string(ev_inc) + " ref=" + std::to_string(ev_ref));
                        return true;
                    }

                    // Test 180-deg flip color symmetry
                    Position flp = flip_colors_180(pos);
                    int ev_flp = nnue::evaluate_position(net, flp);
                    if (ev_flp != ev_ref) {
                        send_line("nnuetest fail flip180_mismatch game=" + std::to_string(g) + " ply=" + std::to_string(next_p)
                                  + " ref=" + std::to_string(ev_ref) + " flp=" + std::to_string(ev_flp));
                        return true;
                    }

                    // Test PackedSample feature extraction exactness (normal + aug_mirror_h == mirror_horizontal)
                    nnue::PackedSample ps = nnue::pack_position(pos, static_cast<int16_t>(ev_ref), 0, next_p);
                    Position mir = mirror_horizontal(pos);
                    for (int persp = 0; persp < 2; ++persp) {
                        int32_t g_norm[nnue::MAX_ACTIVE_FEATS], l_norm[nnue::MAX_ACTIVE_FEATS];
                        int n_norm = nnue::extract_global_features_packed(ps, persp, false, g_norm, l_norm);
                        std::sort(g_norm, g_norm + n_norm);
                        int b_ref = 0; bool f_ref = false;
                        uint16_t loc_ref[nnue::MAX_ACTIVE_FEATS];
                        int n_ref = nnue::extract_local_features(pos, persp, b_ref, f_ref, loc_ref);
                        if (n_norm != n_ref) {
                            send_line("nnuetest fail packed_count_mismatch");
                            return true;
                        }
                        for (int k = 0; k < n_ref; ++k) {
                            if (g_norm[k] != b_ref * nnue::F_LOCAL + static_cast<int>(loc_ref[k])) {
                                send_line("nnuetest fail packed_feat_mismatch");
                                return true;
                            }
                        }

                        int32_t g_mir[nnue::MAX_ACTIVE_FEATS], l_mir[nnue::MAX_ACTIVE_FEATS];
                        int n_mir = nnue::extract_global_features_packed(ps, persp, true, g_mir, l_mir);
                        std::sort(g_mir, g_mir + n_mir);
                        int b_mref = 0; bool f_mref = false;
                        uint16_t loc_mref[nnue::MAX_ACTIVE_FEATS];
                        int n_mref = nnue::extract_local_features(mir, persp, b_mref, f_mref, loc_mref);
                        if (n_mir != n_mref) {
                            send_line("nnuetest fail packed_mirror_count_mismatch");
                            return true;
                        }
                        for (int k = 0; k < n_mref; ++k) {
                            if (g_mir[k] != b_mref * nnue::F_LOCAL + static_cast<int>(loc_mref[k])) {
                                send_line("nnuetest fail packed_mirror_feat_mismatch");
                                return true;
                            }
                        }
                    }
                }
            }
            send_line("nnuetest ok plies=" + std::to_string(total_plies));
            return true;
        }
        if (cmd == "piecevalues") {
            std::lock_guard<std::mutex> lock(out_mutex_);
            print_piecevalues(std::cout);
            std::cout << std::flush;
            return true;
        }
        if (cmd == "evalterms" || cmd.rfind("evalterms ", 0) == 0) {
            stop_active_search();
            Position pos = current_state_.position;
            if (cmd.size() > 10) {
                std::string lppn_str = cmd.substr(10);
                size_t p = lppn_str.find_first_not_of(" \t");
                if (p != std::string::npos) {
                    lppn_str = lppn_str.substr(p);
                    int ply = 0;
                    std::string err;
                    if (!parse_lppn(lppn_str, pos, ply, err, true)) {
                        send_error(err);
                        return true;
                    }
                }
            }
            std::lock_guard<std::mutex> lock(out_mutex_);
            print_evalterms(pos, std::cout);
            std::cout << std::flush;
            return true;
        }
        if (cmd.rfind("reloadparams ", 0) == 0) {
            stop_active_search();
            std::string path = cmd.substr(13);
            size_t p = path.find_first_not_of(" \t");
            if (p != std::string::npos) path = path.substr(p);
            if (!load_eval_params_file(path)) {
                send_error("Failed to load params file");
                return true;
            }
            searcher_.clear_tt();
            send_line("paramsok");
            return true;
        }
        if (cmd.rfind("profile ", 0) == 0) {
            stop_active_search();
            std::string sub = cmd.substr(8);
            size_t p = sub.find_first_not_of(" \t");
            if (p != std::string::npos) sub = sub.substr(p);
            SearchProfile& prof = get_search_profile();
            if (sub == "on") {
                prof.enabled = true;
                prof.reset();
                send_line("profileok on");
            } else if (sub == "off") {
                prof.enabled = false;
                send_line("profileok off");
            } else if (sub == "reset") {
                prof.reset();
                send_line("profileok reset");
            } else if (sub == "report") {
                std::ostringstream oss;
                oss << "profile report"
                    << " total_ns=" << prof.total_ns
                    << " eval_total_ns=" << prof.eval_total_ns
                    << " hanging_ns=" << prof.hanging_ns
                    << " movegen_ns=" << prof.movegen_ns
                    << " qsearch_ns=" << prof.qsearch_ns
                    << " tt_ns=" << prof.tt_ns
                    << " eval_calls=" << prof.eval_calls
                    << " eval_lazy_skips=" << prof.eval_lazy_skips
                    << " eval_tt_hits=" << prof.eval_tt_hits
                    << " hanging_calls=" << prof.hanging_calls
                    << " movegen_legal_calls=" << prof.movegen_legal_calls
                    << " movegen_cap_calls=" << prof.movegen_cap_calls
                    << " qsearch_calls=" << prof.qsearch_calls
                    << " tt_probes=" << prof.tt_probes
                    << " tt_stores=" << prof.tt_stores
                    << " score_moves_ns=" << prof.score_moves_ns
                    << " score_moves_calls=" << prof.score_moves_calls
                    << " see_ns=" << prof.see_ns
                    << " see_calls=" << prof.see_calls
                    << " mate1_ns=" << prof.mate1_ns
                    << " mate1_calls=" << prof.mate1_calls
                    << " apply_undo_ns=" << prof.apply_undo_ns
                    << " pick_ns=" << prof.pick_ns;
                send_line(oss.str());
            } else {
                send_error("Unknown profile subcommand");
            }
            return true;
        }
        if (cmd.rfind("stagedcheck ", 0) == 0) {
            stop_active_search();
            std::string lppn_str = cmd.substr(12);
            size_t p = lppn_str.find_first_not_of(" \t");
            if (p == std::string::npos) {
                send_error("Missing LPPN for stagedcheck");
                return true;
            }
            lppn_str = lppn_str.substr(p);
            Position pos;
            int ply = 0;
            std::string err;
            if (!parse_lppn(lppn_str, pos, ply, err, true)) {
                send_error(err);
                return true;
            }
            MoveList ref_legal;
            generate_legal_moves(pos, ref_legal);

            // Construct diverse test tuples of (tt_move, killer1, killer2, counter_move)
            Move cap_m = Move::none();
            Move quiet_m = Move::none();
            Move drop_m = Move::none();
            Move rot_m = Move::none();
            for (int i = 0; i < ref_legal.count; ++i) {
                const Move& m = ref_legal.moves[i];
                if (is_capture_move(pos, m) && cap_m.is_none()) cap_m = m;
                else if (m.kind == MoveKind::Drop && drop_m.is_none()) drop_m = m;
                else if (m.kind == MoveKind::Rotate && rot_m.is_none()) rot_m = m;
                else if (!is_capture_move(pos, m) && quiet_m.is_none()) quiet_m = m;
            }
            Move bogus1{MoveKind::Single, 0, 80, VUONG, -1, 0};
            Move bogus2{MoveKind::Drop, -1, 40, BINH, 2, 0};
            Move bogus3{MoveKind::Team, 10, 20, LONG, -1, 1};

            struct Trial { Move tt, k1, k2, cm; };
            Trial trials[6] = {
                {Move::none(), Move::none(), Move::none(), Move::none()},
                {ref_legal.count > 0 ? ref_legal.moves[0] : Move::none(), quiet_m, rot_m, drop_m},
                {cap_m, drop_m, quiet_m, rot_m},
                {quiet_m, quiet_m, cap_m, cap_m},
                {bogus1, bogus2, bogus3, bogus1},
                {ref_legal.count > 0 ? ref_legal.moves[ref_legal.count - 1] : Move::none(), bogus2, quiet_m, bogus3},
            };

            for (int ti = 0; ti < 6; ++ti) {
                MoveList staged;
                Position pos_copy = pos;
                searcher_.collect_staged_moves(pos_copy, trials[ti].tt, trials[ti].k1, trials[ti].k2, trials[ti].cm, staged);
                if (staged.count != ref_legal.count) {
                    std::string extra = " ti=" + std::to_string(ti) + " missing=";
                    for (int r = 0; r < ref_legal.count; ++r) {
                        bool f = false;
                        for (int s = 0; s < staged.count; ++s) {
                            if (staged.moves[s] == ref_legal.moves[r]) { f = true; break; }
                        }
                        if (!f) extra += format_canonical_notation(pos, ref_legal.moves[r]) + ",";
                    }
                    send_line("stagedcheck fail count " + std::to_string(staged.count) + " vs " + std::to_string(ref_legal.count) + extra);
                    return true;
                }
                // Check 0 duplicates and 100% membership in ref_legal
                for (int i = 0; i < staged.count; ++i) {
                    for (int j = i + 1; j < staged.count; ++j) {
                        if (staged.moves[i] == staged.moves[j]) {
                            send_line("stagedcheck fail duplicate");
                            return true;
                        }
                    }
                    bool found = false;
                    for (int r = 0; r < ref_legal.count; ++r) {
                        if (staged.moves[i] == ref_legal.moves[r]) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        send_line("stagedcheck fail illegal_or_extra");
                        return true;
                    }
                }
            }
            send_line("stagedcheck ok " + std::to_string(ref_legal.count));
            return true;
        }
        if (cmd.rfind("qcheck ", 0) == 0) {
            stop_active_search();
            std::string lppn_str = cmd.substr(7);
            size_t p = lppn_str.find_first_not_of(" \t");
            if (p == std::string::npos) {
                send_error("Missing LPPN for qcheck");
                return true;
            }
            lppn_str = lppn_str.substr(p);
            Position pos;
            int ply = 0;
            std::string err;
            if (!parse_lppn(lppn_str, pos, ply, err, true)) {
                send_error(err);
                return true;
            }
            bool in_chk = is_in_check(pos, static_cast<Side>(pos.side_to_move));
            int ev = evaluate(pos);
            Position leaf_pos;
            int qply_len = 0;
            int qs = searcher_.extract_qleaf(pos, leaf_pos, qply_len);
            std::string leaf_lppn;
            if (!format_lppn(leaf_pos, ply + qply_len, leaf_lppn, err, false)) {
                send_error(err);
                return true;
            }
            std::ostringstream oss;
            oss << "qcheck " << (in_chk ? 1 : 0) << ' ' << ev << ' ' << qs << ' ' << qply_len << ' ' << leaf_lppn;
            send_line(oss.str());
            return true;
        }
        if (cmd.rfind("qleaf ", 0) == 0) {
            stop_active_search();
            std::string rest = cmd.substr(6);
            size_t p = rest.find_first_not_of(" \t");
            if (p == std::string::npos) {
                send_error("Missing arguments for qleaf");
                return true;
            }
            rest = rest.substr(p);
            size_t last_sp = rest.find_last_of(" \t");
            if (last_sp == std::string::npos) {
                send_error("Missing move notation for qleaf");
                return true;
            }
            std::string lppn_str = rest.substr(0, last_sp);
            std::string move_str = rest.substr(last_sp + 1);
            size_t lp_end = lppn_str.find_last_not_of(" \t");
            if (lp_end != std::string::npos) lppn_str.resize(lp_end + 1);

            Position pos;
            int ply = 0;
            std::string err;
            if (!parse_lppn(lppn_str, pos, ply, err, true)) {
                send_error(err);
                return true;
            }
            Move mv;
            if (!parse_canonical_notation(pos, move_str, mv, err)) {
                send_error("illegal move");
                return true;
            }
            UndoState u;
            apply_move(pos, mv, u);
            ++ply;

            Position leaf_pos;
            int qply_len = 0;
            int qs = searcher_.extract_qleaf(pos, leaf_pos, qply_len);
            std::string leaf_lppn;
            if (!format_lppn(leaf_pos, ply + qply_len, leaf_lppn, err, false)) {
                send_error(err);
                return true;
            }
            std::ostringstream oss;
            oss << "qleaf " << (-qs) << ' ' << qply_len << ' ' << leaf_lppn;
            send_line(oss.str());
            return true;
        }
        if (cmd == "bench" || cmd.rfind("bench ", 0) == 0) {
            stop_active_search();
            int depth = 6;
            int movetime_ms = 1000;
            std::istringstream iss(cmd);
            std::string tok;
            iss >> tok;
            if (iss >> depth) {
                iss >> movetime_ms;
            }
            run_bench(depth, movetime_ms);
            // run_bench forces a 64 MB TT for reproducible numbers; restore the user's Hash setting.
            searcher_.resize_tt(opt_hash_mb_);
            searcher_.clear_tt();
            return true;
        }

        send_error("Unknown command");
        return true;
    }

    void run_bench(int max_depth = 6, int movetime_ms = 1000) {
        struct BenchItem {
            const char* name;
            const char* lppn;
        };
        static const BenchItem BENCH_SUITE[] = {
            {"startpos", nullptr},
            {"S3", "lppn1 V23L24/9/9/9/9/9/5s03/3b01b03/3t0v0t03 x -/- -/B 10"},
            {"S8", "lppn1 4V24/9/8B2/9/9/9/9/t07l0/4v04 c -/- -/- 20"},
            {"T01", "lppn1 X2K2S2T2V2T21K2X2/9/B2B2B2B2B21B2B2B2/4L21[B2P2]2/3S35/9/b0[b0l0]b0b0b0b0b0b0b0/3s0p0s0t02/x0k01t01v01k0x0 x -/- -/- 15"},
            {"T02", "lppn1 X2K21T2V24/1S22T14/B2B2B2B2B21K1B2[B2X2]/4L24/3S32[B2P2B2]2/9/b0[b0l0]b0[b0p0]b0b0b0b0b0/3s02t02/x0k01t0k01s0v0x0 x -/- -/- 27"},
            {"T03", "lppn1 X2K21T2V24/1S22T11p02/B2B2B2B2B21K11[B2X2]/4S22L21/6[B2P2]2/6B22/b0[b0l0]b0b0b0[b0k0]b0b0b0/3s02t02/x0k01t02s0v0x0 x -/- -/B 33"},
            {"T04", "lppn1 X2K21T2V24/1S22T14/B2B2B2B2B24/4[S2P2]2L2p0/6k32/6b02/b0[b0l0]b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- B/KXBBB 45"},
            {"T05", "lppn1 1K2S31B2T2V2K21/4T22S21/B22B2B2B2B2B21/9/2P26/b01k01p01b02/1[b0l0][b0k3]b0b01b0v0b3/3s01L2x32/x04S1T02 c -/- -/TXXBBB 82"},
            {"T06", "lppn1 X2V2T2K0[B2K3]4/1S21T23L21/B2B2B2B2B3[B2S2]B32/5B23/5X23/1b01l01k0p02/b0b0b0b0b01b1b0b0/2[s0t0]1t12s01/[x0x0]1v06 c -/- K/P 58"},
            {"T07", "lppn1 X2V2T21K22K21/1S21T1P22S21/B2B2B2B2B2B2B2B21/7X2B2/3b05/5k23/b0b0b0b0b01b02/1v0p01t02l01/x0s11t0b11s0k01 c -/- LB/X 48"},
            {"T08", "lppn1 X2V2T2K0B24/1S21T25/B2B2B2B2B3B2[B3L2]2/2P2K31B23/9/b0b02b04/1b0b0b01s0B31b0/2s0t1x12s01/[x0x0]1v02l03 x -/- TKB/KP 87"},
            {"T09", "lppn1 X2V2T21K22K21/1S21T1P22S21/B2B2B2B2B2B2B2B21/8B2/3b05/5k23/b0b0b0b0b01b02/1v0p01t02s01/x0s11t0b12k01 c -/- LLB/XX 50"},
            {"T10", "lppn1 X2K21T2V24/1S22T11p02/B2B2B2B2B21K1p01/4S22L2[B2X2]/6k32/6b02/b0[b0l0]b0b0b0b0t0b0b0/3s05/x0k01t02s0v0x0 x -/- B/BB 41"},
        };

        uint64_t grand_total_nodes = 0;
        int64_t grand_total_time_ms = 0;

        send_line("========================================================================================================");
        send_line("Pos       Depth   Nodes       Time(ms)   NPS         EBF       FirstCutoff%   TTHit%       BestMove");
        send_line("--------------------------------------------------------------------------------------------------------");

        for (const auto& item : BENCH_SUITE) {
            // newgame between positions per spec requirement 5
            searcher_.resize_tt(64);
            searcher_.clear_tt();

            GameState st;
            if (item.lppn == nullptr) {
                Position pos;
                setup_initial_position(pos);
                st = make_base_state(pos, 0);
            } else {
                Position pos;
                int ply = 0;
                std::string err;
                if (!parse_lppn(item.lppn, pos, ply, err, true)) {
                    send_error("Bench failed parsing LPPN for " + std::string(item.name));
                    continue;
                }
                st = make_base_state(pos, ply);
            }

            SearchLimits limits;
            limits.id = 1;
            limits.max_depth = max_depth;
            limits.max_nodes = 50000000ULL;
            limits.movetime_ms = movetime_ms;
            limits.multipv = 1;
            limits.infinite = false;

            std::vector<uint64_t> iter_nodes(max_depth + 4, 0);
            auto on_info = [&iter_nodes](const PVLine& line) {
                if (line.depth >= 1 && static_cast<size_t>(line.depth) < iter_nodes.size()) {
                    iter_nodes[line.depth] = line.nodes;
                }
            };

            std::atomic<bool> stop_flag{false};
            int64_t t0 = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            SearchResult res = searcher_.search(st, limits, stop_flag, on_info);
            int64_t t1 = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t time_ms = std::max<int64_t>(1, t1 - t0);

            int depth_reached = res.lines.empty() ? 1 : res.lines[0].depth;
            uint64_t nodes = res.total_nodes;
            grand_total_nodes += nodes;
            grand_total_time_ms += time_ms;
            uint64_t nps = (nodes * 1000ULL) / time_ms;

            double ebf = 0.0;
            if (depth_reached >= 2 && iter_nodes[depth_reached - 1] > 0) {
                ebf = static_cast<double>(iter_nodes[depth_reached]) / iter_nodes[depth_reached - 1];
            }

            std::string first_cutoff_str = "n/a";
            std::string tt_hit_str = "n/a";
#ifdef HOMEI_STATS
            const auto& s = searcher_.stats();
            if (s.beta_cutoffs_total > 0) {
                double pct = (100.0 * s.beta_cutoffs_first_move) / s.beta_cutoffs_total;
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.1f%%", pct);
                first_cutoff_str = buf;
            } else {
                first_cutoff_str = "0.0%";
            }
            if (s.tt_probe > 0) {
                double pct = (100.0 * s.tt_hit) / s.tt_probe;
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.1f%%", pct);
                tt_hit_str = buf;
            } else {
                tt_hit_str = "0.0%";
            }
#endif

            std::string bm_not = res.best_move.is_none() ? "none" : format_canonical_notation(st.position, res.best_move);

            char row[256];
            std::snprintf(row, sizeof(row),
                "%-10s%-8d%-12llu%-11lld%-12llu%-10.2f%-15s%-13s%s",
                item.name, depth_reached, (unsigned long long)nodes, (long long)time_ms,
                (unsigned long long)nps, ebf, first_cutoff_str.c_str(), tt_hit_str.c_str(), bm_not.c_str());
            send_line(row);
        }

        uint64_t overall_nps = (grand_total_time_ms > 0) ? ((grand_total_nodes * 1000ULL) / grand_total_time_ms) : 0;
        char sum_buf[256];
        std::snprintf(sum_buf, sizeof(sum_buf),
            "--------------------------------------------------------------------------------------------------------\n"
            "Total: 13 positions, Nodes: %llu, Time: %lld ms, NPS: %llu\n"
            "========================================================================================================",
            (unsigned long long)grand_total_nodes, (long long)grand_total_time_ms, (unsigned long long)overall_nps);
        send_line(sum_buf);
        send_line("bench done " + std::to_string(grand_total_nodes) + " nodes " + std::to_string(grand_total_time_ms) + " ms");
    }

    void search_worker_loop() {
        struct BusyResetGuard {
            std::mutex& mtx;
            std::condition_variable& cv;
            bool& busy;
            ~BusyResetGuard() {
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    busy = false;
                }
                cv.notify_all();
            }
        };

        while (true) {
            GameState state_copy;
            SearchLimits limits;
            {
                std::unique_lock<std::mutex> lk(worker_mutex_);
                worker_cv_.wait(lk, [this]() { return worker_has_job_ || worker_exit_; });
                if (worker_exit_ && !worker_has_job_) {
                    return;
                }
                state_copy = std::move(worker_state_);
                limits = worker_limits_;
                worker_has_job_ = false;
            }

            {
                BusyResetGuard guard{worker_mutex_, worker_done_cv_, worker_busy_};
                execute_search(state_copy, limits);
            }
        }
    }

    void execute_search(const GameState& state_copy, const SearchLimits& limits) {
        // Fast verified Opening Book lookup (with rep3 protection & configurable BookDepth/BookRandom/BookFile)
        if (opt_own_book_ && opt_book_depth_ > 0 && !limits.infinite && limits.movetime_ms > 0 &&
            limits.max_depth >= 12 && limits.max_nodes >= 10000000ULL &&
            limits.multipv == 1 && state_copy.ply < opt_book_depth_ &&
            state_copy.result.status == GameStatus::Ongoing) {
            Move book_mv = Move::none();
            int book_sc = 0;
            book::ProbeOptions bopts;
            bopts.random_pick = opt_book_random_;
            bopts.seed_salt = static_cast<uint64_t>(limits.id) * 0x517CC1B727220A95ULL;
            if (opening_book_.probe(state_copy, book_mv, book_sc, bopts)) {
                JsonValue info_obj = JsonValue::make_object();
                info_obj.obj_val["schema"] = JsonValue::make_string("clp.info/1");
                info_obj.obj_val["id"] = JsonValue::make_int(limits.id);
                info_obj.obj_val["multipv"] = JsonValue::make_int(1);
                info_obj.obj_val["depth"] = JsonValue::make_int(1);
                info_obj.obj_val["time_ms"] = JsonValue::make_int(1);
                info_obj.obj_val["nodes"] = JsonValue::make_int(1);
                JsonValue score_obj = JsonValue::make_object();
                score_obj.obj_val["perspective"] = JsonValue::make_string("root");
                score_obj.obj_val["kind"] = JsonValue::make_string("cp");
                score_obj.obj_val["value"] = JsonValue::make_int(book_sc);
                info_obj.obj_val["score"] = std::move(score_obj);
                JsonValue pv_arr = JsonValue::make_array();
                std::string not_str = format_canonical_notation(state_copy.position, book_mv);
                pv_arr.arr_val.push_back(encode_move_json(book_mv, not_str));
                info_obj.obj_val["pv"] = std::move(pv_arr);
                send_line("info id " + std::to_string(limits.id) + " info64 " + base64url_encode(json_stringify(info_obj)));
                std::string move_json = format_move_json_str(book_mv, not_str);
                send_line("bestmove id " + std::to_string(limits.id) + " move64 " + base64url_encode(move_json));
                return;
            }
        }

        auto on_info = [this, &state_copy, &limits](const PVLine& line) {
            JsonValue info_obj = JsonValue::make_object();
            info_obj.obj_val["schema"] = JsonValue::make_string("clp.info/1");
            info_obj.obj_val["id"] = JsonValue::make_int(limits.id);
            info_obj.obj_val["multipv"] = JsonValue::make_int(line.multipv_rank);
            info_obj.obj_val["depth"] = JsonValue::make_int(line.depth);
            info_obj.obj_val["time_ms"] = JsonValue::make_int(line.time_ms);
            info_obj.obj_val["nodes"] = JsonValue::make_int(static_cast<int64_t>(line.nodes));

            JsonValue score_obj = JsonValue::make_object();
            score_obj.obj_val["perspective"] = JsonValue::make_string("root");
            if (std::abs(line.score) >= SCORE_MATE - MAX_PLY) {
                int mate_ply = (line.score > 0) ? (SCORE_MATE - line.score) : (SCORE_MATE + line.score);
                int mate_moves = std::max(1, mate_ply);
                int val = (line.score > 0) ? mate_moves : -mate_moves;
                score_obj.obj_val["kind"] = JsonValue::make_string("mate");
                score_obj.obj_val["value"] = JsonValue::make_int(val);
            } else {
                score_obj.obj_val["kind"] = JsonValue::make_string("cp");
                score_obj.obj_val["value"] = JsonValue::make_int(line.score);
            }
            info_obj.obj_val["score"] = std::move(score_obj);

            JsonValue pv_arr = JsonValue::make_array();
            Position sim = state_copy.position;
            for (const Move& m : line.pv) {
                std::string not_str = format_canonical_notation(sim, m);
                pv_arr.arr_val.push_back(encode_move_json(m, not_str));
                UndoState u;
                apply_move(sim, m, u);
            }
            info_obj.obj_val["pv"] = std::move(pv_arr);

            std::string b64 = base64url_encode(json_stringify(info_obj));
            send_line("info id " + std::to_string(limits.id) + " info64 " + b64);
        };

        SearchResult res = searcher_.search(state_copy, limits, stop_flag_, on_info);

        // In infinite mode, wait until stop_flag_ is signaled before emitting bestmove
        if (limits.infinite && !stop_flag_.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_stop_.wait(lk, [this]() {
                return stop_flag_.load(std::memory_order_relaxed);
            });
        }

        if (res.best_move.is_none()) {
            send_line("bestmove id " + std::to_string(limits.id) + " none");
        } else {
            std::string not_str = format_canonical_notation(state_copy.position, res.best_move);
            std::string move_json = format_move_json_str(res.best_move, not_str);
            std::string move64 = base64url_encode(move_json);
            send_line("bestmove id " + std::to_string(limits.id) + " move64 " + move64);
        }
    }

    void start_search(GameState state_copy, SearchLimits limits) {
        stop_flag_.store(false, std::memory_order_relaxed);
        active_id_.store(limits.id, std::memory_order_relaxed);

        if (limits.infinite) {
            send_line("searchstarted id " + std::to_string(limits.id));
        }

        if (!state_copy.repetition_complete) {
            JsonValue msg = JsonValue::make_object();
            msg.obj_val["message"] = JsonValue::make_string("Warning: repetition history incomplete");
            send_line("info id " + std::to_string(limits.id) + " string64 " + base64url_encode(json_stringify(msg)));
        }

        {
            std::lock_guard<std::mutex> lk(worker_mutex_);
            worker_state_ = std::move(state_copy);
            worker_limits_ = limits;
            worker_has_job_ = true;
            worker_busy_ = true;
        }
        worker_cv_.notify_all();
    }
};

} // namespace homei

int main(int argc, char* argv[]) {
    homei::init_homei_tables();
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--piecevalues") {
            homei::print_piecevalues(std::cout);
            return 0;
        }
    }
    homei::EngineApp app;
    app.run();
    return 0;
}

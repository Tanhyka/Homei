#include "adjudicate.h"
#include "attack.h"
#include "base64url.h"
#include "eval.h"
#include "geometry.h"
#include "json.h"
#include "lppn.h"
#include "movegen.h"
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
    }

    ~EngineApp() {
        stop_active_search();
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
    int opt_depth_ = 12;
    uint64_t opt_nodes_ = 50000000ULL;
    int opt_multipv_ = 1;
    int opt_threads_ = 1;

    // Active background search
    std::thread search_thread_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<int> active_id_{0};
    std::mutex cv_mutex_;
    std::condition_variable cv_stop_;

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
        if (search_thread_.joinable()) {
            search_thread_.join();
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
            send_line("option name Depth type spin default 12 min 1 max 32");
            send_line("option name Nodes type spin default 50000000 min 1 max 50000000");
            send_line("option name MultiPV type spin default 1 min 1 max 5");
            send_line("option name Threads type spin default 1 min 1 max 32");
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
            std::istringstream iss(cmd);
            std::string tok_set, tok_name, name, tok_val;
            int64_t val = 0;
            std::string extra;
            if (!(iss >> tok_set >> tok_name >> name >> tok_val >> val) ||
                tok_set != "setoption" || tok_name != "name" || tok_val != "value" || (iss >> extra)) {
                send_error("Invalid setoption syntax");
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
                searcher_.resize_tt(static_cast<size_t>(val));
                send_line("optionok name Hash value " + std::to_string(val));
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
                limits.max_nodes = 50000000ULL;
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
            } else if (mode == "camtime" || mode == "xanhtime" || mode == "btime" || mode == "wtime" || mode == "clock") {
                limits.infinite = false;
                limits.max_depth = opt_depth_;
                limits.max_nodes = opt_nodes_;
                std::string key = mode;
                int64_t val = 0;
                do {
                    if (key == "camtime" || key == "btime") {
                        if (iss >> val) limits.camtime_ms = val;
                    } else if (key == "xanhtime" || key == "wtime") {
                        if (iss >> val) limits.xanhtime_ms = val;
                    } else if (key == "caminc" || key == "binc") {
                        if (iss >> val) limits.caminc_ms = val;
                    } else if (key == "xanhinc" || key == "winc") {
                        if (iss >> val) limits.xanhinc_ms = val;
                    }
                } while (iss >> key);

                Side stm = static_cast<Side>(current_state_.position.side_to_move);
                int64_t time_left = (stm == CAM) ? limits.camtime_ms : limits.xanhtime_ms;
                int64_t inc = (stm == CAM) ? limits.caminc_ms : limits.xanhinc_ms;
                if (time_left <= 0) time_left = 5000;
                limits.optimum_time_ms = std::max<int64_t>(10, time_left / 25 + inc * 3 / 4);
                limits.maximum_time_ms = std::min<int64_t>(3 * limits.optimum_time_ms, time_left / 3);
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
        if (cmd == "piecevalues") {
            std::lock_guard<std::mutex> lock(out_mutex_);
            print_piecevalues(std::cout);
            std::cout << std::flush;
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

        search_thread_ = std::thread([this, state_copy = std::move(state_copy), limits]() mutable {
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
        });
    }
};

} // namespace homei

int main(int argc, char* argv[]) {
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

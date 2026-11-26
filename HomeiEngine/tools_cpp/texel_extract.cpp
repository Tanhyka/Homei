// Fast multithreaded Texel dataset extractor linking homei_core.
// Usage: texel_extract <manifest.tsv> <out_texel.bin> [threads]
// Manifest TSV line: split(0/1) \t cam_wdl(0/0.5/1) \t seed_lppn \t space_separated_moves

#include "attack.h"
#include "eval.h"
#include "eval_v35.h"
#include "geometry.h"
#include "lppn.h"
#include "movegen.h"
#include "notation.h"
#include "search.h"
#include "zobrist.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace homei;

namespace {

struct GameJob {
    uint8_t split = 0; // 0=train, 1=val
    float cam_wdl = 0.5f;
    std::string lppn;
    std::vector<std::string> moves;
};

struct PackedSample {
    uint64_t key = 0;
    uint8_t split = 0;
    uint16_t phase = 0;
    int32_t non_v35_base = 0;
    float stm_wdl = 0.5f;
    int16_t qscore = 0;
    std::vector<v35::V35TraceItem> items;
};

bool parse_compact_move_fallback(const Position& pos, const std::string& tok, Move& out_m) {
    std::string err;
    if (parse_canonical_notation(pos, tok, out_m, err)) {
        return true;
    }
    // Strip optional trailing !/? annotation
    std::string cleaned = tok;
    while (!cleaned.empty() && (cleaned.back() == '+' || cleaned.back() == '!' || cleaned.back() == '?')) {
        cleaned.pop_back();
    }
    if (cleaned != tok && parse_canonical_notation(pos, cleaned, out_m, err)) {
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: texel_extract <manifest.tsv> <out_texel.bin> [threads]\n";
        return 1;
    }

    init_zobrist();
    init_geometry();
    init_attack_tables();

    std::string manifest_path = argv[1];
    std::string out_path = argv[2];
    int num_threads = (argc >= 4) ? std::max(1, std::atoi(argv[3])) : 6;

    std::vector<GameJob> jobs;
    {
        std::ifstream ifs(manifest_path);
        if (!ifs.is_open()) {
            std::cerr << "Failed to open manifest: " << manifest_path << "\n";
            return 1;
        }
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.empty()) continue;
            size_t t1 = line.find('\t');
            size_t t2 = (t1 != std::string::npos) ? line.find('\t', t1 + 1) : std::string::npos;
            size_t t3 = (t2 != std::string::npos) ? line.find('\t', t2 + 1) : std::string::npos;
            if (t1 == std::string::npos || t2 == std::string::npos || t3 == std::string::npos) continue;

            GameJob j;
            j.split = static_cast<uint8_t>(std::atoi(line.substr(0, t1).c_str()) != 0 ? 1 : 0);
            j.cam_wdl = std::stof(line.substr(t1 + 1, t2 - t1 - 1));
            j.lppn = line.substr(t2 + 1, t3 - t2 - 1);
            std::istringstream mss(line.substr(t3 + 1));
            std::string mv_s;
            while (mss >> mv_s) {
                j.moves.push_back(mv_s);
            }
            if (!j.lppn.empty()) {
                jobs.push_back(std::move(j));
            }
        }
    }

    std::cerr << "[texel_extract] Loaded " << jobs.size() << " games, running with " << num_threads << " threads...\n";
    auto t0 = std::chrono::steady_clock::now();

    std::atomic<size_t> next_job{0};
    std::vector<std::vector<PackedSample>> thread_samples(num_threads);

    auto worker = [&](int tid) {
        Searcher searcher;
        searcher.resize_tt(8);
        std::vector<PackedSample>& local_out = thread_samples[tid];
        local_out.reserve(32768);

        while (true) {
            size_t jid = next_job.fetch_add(1);
            if (jid >= jobs.size()) break;

            const GameJob& job = jobs[jid];
            Position pos;
            int start_ply = 0;
            std::string err;
            if (!parse_lppn(job.lppn, pos, start_ply, err, false)) {
                continue;
            }

            int ply_rel = 0;
            for (const std::string& mtok : job.moves) {
                Move mv;
                if (!parse_compact_move_fallback(pos, mtok, mv)) {
                    break;
                }
                UndoState u;
                apply_move(pos, mv, u);
                ++ply_rel;

                // Sample every 2nd ply from ply_rel >= 1 (including early post-reserve drops from startpos!)
                // Skip trivial or already in-check positions before qleaf
                int abs_ply = start_ply + ply_rel;
                if (abs_ply < 2 || (ply_rel % 2 != 0)) {
                    continue;
                }
                if (is_in_check(pos, static_cast<Side>(pos.side_to_move))) {
                    continue;
                }

                Position qleaf;
                int qply_len = 0;
                int qscore = searcher.extract_qleaf(pos, qleaf, qply_len);
                if (std::abs(qscore) > 2200) continue;
                Side leaf_stm = static_cast<Side>(qleaf.side_to_move);
                if (is_in_check(qleaf, leaf_stm)) continue;

                v35::V35Trace trace;
                int non_v35 = 0;
                int ev_total = evaluate_v35_extract(qleaf, non_v35, trace);
                (void)ev_total;

                PackedSample ps;
                ps.key = qleaf.zobrist;
                ps.split = job.split;
                ps.phase = static_cast<uint16_t>(trace.phase);
                ps.non_v35_base = non_v35;
                ps.stm_wdl = (leaf_stm == CAM) ? job.cam_wdl : (1.0f - job.cam_wdl);
                ps.qscore = static_cast<int16_t>(std::clamp(qscore, -30000, 30000));
                ps.items.assign(trace.items, trace.items + trace.count);
                local_out.push_back(std::move(ps));
            }
        }
    };

    std::vector<std::thread> workers;
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back(worker, t);
    }
    for (auto& th : workers) th.join();

    // Deduplicate across train/val strictly by zobrist (never leak train into val)
    std::unordered_set<uint64_t> seen_train;
    std::unordered_set<uint64_t> seen_val;
    seen_train.reserve(250000);
    seen_val.reserve(60000);

    std::ofstream ofs(out_path, std::ios::binary);
    if (!ofs.is_open()) {
        std::cerr << "Failed to open output: " << out_path << "\n";
        return 1;
    }

    uint32_t magic = 0x56333554; // "V35T"
    uint32_t n_feats = static_cast<uint32_t>(v35::V35_TOTAL_FEATS);
    ofs.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    ofs.write(reinterpret_cast<const char*>(&n_feats), sizeof(n_feats));

    size_t n_train = 0, n_val = 0;
    for (const auto& vec : thread_samples) {
        for (const auto& s : vec) {
            if (s.split == 0) {
                if (seen_val.count(s.key) || !seen_train.insert(s.key).second) continue;
                ++n_train;
            } else {
                if (seen_train.count(s.key) || !seen_val.insert(s.key).second) continue;
                ++n_val;
            }
            uint8_t sp = s.split;
            uint16_t ph = s.phase;
            int32_t bs = s.non_v35_base;
            float wdl = s.stm_wdl;
            int16_t qs = s.qscore;
            uint16_t ni = static_cast<uint16_t>(s.items.size());
            ofs.write(reinterpret_cast<const char*>(&sp), sizeof(sp));
            ofs.write(reinterpret_cast<const char*>(&ph), sizeof(ph));
            ofs.write(reinterpret_cast<const char*>(&bs), sizeof(bs));
            ofs.write(reinterpret_cast<const char*>(&wdl), sizeof(wdl));
            ofs.write(reinterpret_cast<const char*>(&qs), sizeof(qs));
            ofs.write(reinterpret_cast<const char*>(&ni), sizeof(ni));
            for (const auto& it : s.items) {
                ofs.write(reinterpret_cast<const char*>(&it.idx), sizeof(it.idx));
                ofs.write(reinterpret_cast<const char*>(&it.val), sizeof(it.val));
            }
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::cerr << "[texel_extract] Done in " << secs << "s: train=" << n_train << ", val=" << n_val << " quiet samples.\n";
    return 0;
}

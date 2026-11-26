#include "book.h"
#include "attack.h"
#include "eval.h"
#include "json.h"
#include "lppn.h"
#include "movegen.h"
#include "notation.h"
#include "zobrist.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>

namespace homei::book {

namespace {

inline int history_count_for_hash(const GameState& state, uint64_t h) {
    int cnt = 0;
    for (const auto& re : state.history) {
        if (re.hash == h) {
            if (re.count > cnt) cnt = re.count;
        }
    }
    return cnt;
}

inline uint8_t mirror_facing(uint8_t f) {
    if (f == 1) return 3;
    if (f == 3) return 1;
    return f;
}

inline Move mirror_move_h(const Move& m) {
    Move r = m;
    if (r.src >= 0 && r.src < NUM_SQUARES) {
        r.src = static_cast<int8_t>(make_sq(sq_row(r.src), 8 - sq_col(r.src)));
    }
    if (r.dst >= 0 && r.dst < NUM_SQUARES) {
        r.dst = static_cast<int8_t>(make_sq(sq_row(r.dst), 8 - sq_col(r.dst)));
    }
    if (r.facing >= 0) {
        r.facing = static_cast<int8_t>(mirror_facing(static_cast<uint8_t>(r.facing)));
    }
    if (r.rotate_after == 1) r.rotate_after = 3;
    else if (r.rotate_after == 3) r.rotate_after = 1;
    return r;
}

bool probe_entries(
    const BookEntry* entries,
    size_t count,
    const GameState& state,
    Move& out_move,
    int& out_score,
    const ProbeOptions& opts
) {
    if (!entries || count == 0) return false;
    const Position& pos = state.position;
    uint64_t key = pos.zobrist;

    // Lower bound binary search on sorted entries[0..count)
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        if (entries[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= count || entries[lo].key != key) return false;

    const bool in_chk = is_in_check(pos, static_cast<Side>(pos.side_to_move));
    Position sim = pos;

    struct Cand {
        Move mv;
        int16_t score;
        uint16_t weight;
    };
    Cand cands[32]{};
    int n_cands = 0;
    int16_t best_sc = -30000;

    for (size_t i = lo; i < count && entries[i].key == key && n_cands < 32; ++i) {
        if (entries[i].weight < opts.min_weight || entries[i].weight == 0) continue;
        Move m = unpack_move(entries[i].packed_move);
        if (!is_pseudo_legal_move(sim, m)) continue;
        if (!check_move_leaves_king_safe(sim, m, in_chk)) continue;

        // Avoid 3-fold repetition loss (Điều 4 Thiên Nhật Thủ):
        // if playing `m` reaches a position already seen >= 2 times in state.history, reject it!
        UndoState u;
        apply_move(sim, m, u);
        uint64_t next_hash = sim.zobrist;
        undo_move(sim, m, u);
        if (history_count_for_hash(state, next_hash) >= 2) {
            continue;
        }

        cands[n_cands++] = {m, entries[i].score, entries[i].weight};
        if (entries[i].score > best_sc) best_sc = entries[i].score;
    }
    if (n_cands == 0) return false;

    if (!opts.random_pick) {
        // Deterministic mode: pick highest weight, tie-break by highest score.
        int best_idx = 0;
        for (int i = 1; i < n_cands; ++i) {
            if (cands[i].weight > cands[best_idx].weight ||
                (cands[i].weight == cands[best_idx].weight && cands[i].score > cands[best_idx].score)) {
                best_idx = i;
            }
        }
        out_move = cands[best_idx].mv;
        out_score = cands[best_idx].score;
        return true;
    }

    // Filter to candidates within `opts.score_window` cp of the best valid book move
    uint32_t total_w = 0;
    int kept[32]{};
    int n_kept = 0;
    for (int i = 0; i < n_cands; ++i) {
        if (cands[i].score >= best_sc - opts.score_window) {
            kept[n_kept++] = i;
            total_w += cands[i].weight;
        }
    }
    if (n_kept == 0 || total_w == 0) {
        out_move = cands[0].mv;
        out_score = cands[0].score;
        return true;
    }

    static const uint64_t s_proc_seed = []() -> uint64_t {
        uint64_t t = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count()
        );
        std::random_device rd;
        uint64_t r = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
        uint64_t s = t ^ r ^ 0x9E3779B97F4A7C15ULL;
        s ^= s >> 30; s *= 0xBF58476D1CE4E5B9ULL;
        s ^= s >> 27; s *= 0x94D049BB133111EBULL;
        return s ^ (s >> 31);
    }();
    static std::atomic<uint64_t> s_rng_ctr{0x9E3779B97F4A7C15ULL};
    uint64_t h = key ^ opts.seed_salt ^ s_proc_seed ^ s_rng_ctr.fetch_add(0x9E3779B97F4A7C15ULL, std::memory_order_relaxed);
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;

    uint32_t pick = static_cast<uint32_t>(h % total_w);
    uint32_t acc = 0;
    for (int k = 0; k < n_kept; ++k) {
        const Cand& c = cands[kept[k]];
        acc += c.weight;
        if (pick < acc) {
            out_move = c.mv;
            out_score = c.score;
            return true;
        }
    }
    out_move = cands[kept[0]].mv;
    out_score = cands[kept[0]].score;
    return true;
}

void sort_and_dedup(std::vector<BookEntry>& entries) {
    std::sort(entries.begin(), entries.end(), [](const BookEntry& a, const BookEntry& b) {
        if (a.key != b.key) return a.key < b.key;
        if (a.packed_move != b.packed_move) return a.packed_move < b.packed_move;
        return a.weight > b.weight;
    });
    // Deduplicate identical (key, packed_move) keeping highest weight
    size_t write_idx = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (write_idx > 0 &&
            entries[write_idx - 1].key == entries[i].key &&
            entries[write_idx - 1].packed_move == entries[i].packed_move) {
            continue;
        }
        entries[write_idx++] = entries[i];
    }
    entries.resize(write_idx);
    // Final sort by (key, -weight, -score) so highest-weight entries sit first per key
    std::sort(entries.begin(), entries.end(), [](const BookEntry& a, const BookEntry& b) {
        if (a.key != b.key) return a.key < b.key;
        if (a.weight != b.weight) return a.weight > b.weight;
        if (a.score != b.score) return a.score > b.score;
        return a.packed_move < b.packed_move;
    });
}

} // namespace

void OpeningBook::clear() {
    loaded_external_ = false;
    loaded_path_.clear();
    external_entries_.clear();
}

size_t OpeningBook::entry_count() const {
    return loaded_external_ ? external_entries_.size() : static_cast<size_t>(std::max(0, BOOK_COUNT));
}

bool OpeningBook::load_file(const std::string& path, std::string& err) {
    if (path.empty()) {
        clear();
        return true;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        err = "Cannot open book file: " + path;
        return false;
    }

    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();

    // Strip UTF-8 BOM if present
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    size_t first = content.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        err = "Book file is empty";
        return false;
    }

    std::vector<BookEntry> parsed;
    if (content[first] == '{') {
        // GUI JSON format: { "lppn1 <board> <turn> <res> <pris>": [ {"move": "...", "weight": 100, "score": 20, ...}, ... ] }
        JsonValue root;
        if (!json_parse(content, root, err) || !root.is_object()) {
            if (err.empty()) err = "Invalid JSON opening book root";
            return false;
        }
        parsed.reserve(root.obj_val.size() * 4);
        for (const auto& [lppn_key, arr] : root.obj_val) {
            if (!arr.is_array()) continue;
            // Ensure LPPN has ply token (GUI key omits ply -> append " 0")
            std::string full_lppn = lppn_key;
            {
                std::istringstream ks(lppn_key);
                std::string tok;
                int toks = 0;
                while (ks >> tok) ++toks;
                if (toks == 5) full_lppn += " 0";
            }
            Position pos;
            int ply = 0;
            std::string perr;
            if (!parse_lppn(full_lppn, pos, ply, perr, true)) continue;
            Position mir = mirror_horizontal(pos);
            uint64_t key = pos.zobrist;
            uint64_t mkey = mir.zobrist;

            for (const auto& item : arr.arr_val) {
                if (!item.is_object()) continue;
                const JsonValue* mv_v = item.get("move");
                if (!mv_v || !mv_v->is_string() || mv_v->str_val.empty()) continue;
                Move mv;
                std::string merr;
                if (!parse_canonical_notation(pos, mv_v->str_val, mv, merr)) continue;
                int64_t w = 100;
                if (const JsonValue* wv = item.get("weight"); wv && wv->is_int()) {
                    w = std::clamp<int64_t>(wv->int_val, 0, 65535);
                }
                if (w <= 0) continue;
                int64_t sc = 0;
                if (const JsonValue* sv = item.get("score"); sv && sv->is_int()) {
                    sc = std::clamp<int64_t>(sv->int_val, -30000, 30000);
                }
                parsed.push_back(BookEntry{key, pack_move(mv), static_cast<int16_t>(sc), static_cast<uint16_t>(w)});
                if (mkey != key) {
                    Move mmv = mirror_move_h(mv);
                    parsed.push_back(BookEntry{mkey, pack_move(mmv), static_cast<int16_t>(sc), static_cast<uint16_t>(w)});
                }
            }
        }
    } else {
        // Fast .clpbook text format: "<uint64_key> <hex_or_dec_packed_move> <score> <weight>"
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            uint64_t key = 0;
            std::string pm_tok;
            int sc = 0;
            int w = 0;
            if (!(ls >> key >> pm_tok >> sc >> w)) continue;
            if (w <= 0) continue;
            uint32_t pm = 0;
            try {
                pm = static_cast<uint32_t>(std::stoul(pm_tok, nullptr, 0));
            } catch (...) {
                continue;
            }
            parsed.push_back(BookEntry{
                key,
                pm,
                static_cast<int16_t>(std::clamp(sc, -30000, 30000)),
                static_cast<uint16_t>(std::clamp(w, 1, 65535))
            });
        }
    }

    if (parsed.empty()) {
        err = "No valid book entries parsed from: " + path;
        return false;
    }

    sort_and_dedup(parsed);
    external_entries_ = std::move(parsed);
    loaded_external_ = true;
    loaded_path_ = path;
    return true;
}

bool OpeningBook::probe(const GameState& state, Move& out_move, int& out_score, const ProbeOptions& opts) const {
    if (loaded_external_) {
        return probe_entries(external_entries_.data(), external_entries_.size(), state, out_move, out_score, opts);
    }
    return probe_entries(BOOK_ENTRIES, static_cast<size_t>(std::max(0, BOOK_COUNT)), state, out_move, out_score, opts);
}

bool probe_book(const GameState& state, Move& out_move, int& out_score, const ProbeOptions& opts) {
    return probe_entries(BOOK_ENTRIES, static_cast<size_t>(std::max(0, BOOK_COUNT)), state, out_move, out_score, opts);
}

} // namespace homei::book

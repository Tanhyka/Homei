#pragma once

#include <cstdlib>
#include <string>

namespace homei {

struct EngineFeatures {
    bool mate_root = true;
    bool mate_threat = true;
    bool mate_pvlight = true;
    bool mate1_leaf = true;
    bool koma_eval = true;
    bool koma_search = false;

    // v3-3 Stage 1 features (enabled by default in Homei v3-3)
    bool tt_static_eval = true;
    bool fast_probcut = true;
    bool lazy_eval = true;

    // v3-3 Stage 2 features (enabled by default in Homei v3-3)
    bool staged_movegen = true;
    bool drop_history = true;
    bool koma_search_fast = true;

    // v3-3 Stage 3 features (enabled by default in Homei v3-3)
    bool siege_eval = true;

    // v3-5 Tapered Eval + Symmetric PST + KingDanger + Opening Fixes + OwnBook
    bool tapered_v35_eval = true;
    bool own_book = true;

    static const EngineFeatures& get() {
        static EngineFeatures instance = init();
        return instance;
    }

private:
    static void apply_flag(const std::string& s, const std::string& name, bool& field) {
        if (s.find("-" + name) != std::string::npos ||
            s.find("no_" + name) != std::string::npos ||
            s.find(name + "=0") != std::string::npos) {
            field = false;
            return;
        }
        if (s.find("+" + name) != std::string::npos ||
            s.find(name + "=1") != std::string::npos) {
            field = true;
            return;
        }
        // Check exact standalone token match in comma-separated list
        size_t pos = 0;
        while ((pos = s.find(name, pos)) != std::string::npos) {
            bool left_ok = (pos == 0 || s[pos - 1] == ',' || s[pos - 1] == ' ');
            size_t end = pos + name.size();
            bool right_ok = (end == s.size() || s[end] == ',' || s[end] == ' ');
            if (left_ok && right_ok) {
                field = true;
                return;
            }
            pos = end;
        }
    }

    static EngineFeatures init() {
        EngineFeatures f;
        const char* env = std::getenv("HOMEI_FEATURES");
        if (!env) {
            return f;
        }
        std::string s(env);
        if (s == "none" || s == "0") {
            f.mate_root = false;
            f.mate_threat = false;
            f.mate_pvlight = false;
            f.mate1_leaf = false;
            f.koma_eval = false;
            f.koma_search = false;
            f.tt_static_eval = false;
            f.fast_probcut = false;
            f.lazy_eval = false;
            f.staged_movegen = false;
            f.drop_history = false;
            f.koma_search_fast = false;
            f.siege_eval = false;
            f.tapered_v35_eval = false;
            return f;
        }
        if (s == "all" || s == "1") {
            f.koma_eval = true;
            f.koma_search = true;
            f.tt_static_eval = true;
            f.fast_probcut = true;
            f.lazy_eval = true;
            f.staged_movegen = true;
            f.drop_history = true;
            f.koma_search_fast = true;
            f.siege_eval = true;
            f.tapered_v35_eval = true;
            return f;
        }
        bool has_negation = (s.find('-') != std::string::npos || s.find("no_") != std::string::npos || s.find("=0") != std::string::npos);
        if (!has_negation && s.find('+') == std::string::npos &&
            (s.find("mate_") != std::string::npos || s.find("mate1_") != std::string::npos)) {
            // Whitelist mode: only enable features explicitly listed
            f.mate_root = (s.find("mate_root") != std::string::npos);
            f.mate_threat = (s.find("mate_threat") != std::string::npos);
            f.mate_pvlight = (s.find("mate_pvlight") != std::string::npos);
            f.mate1_leaf = (s.find("mate1_leaf") != std::string::npos);
            f.koma_eval = (s.find("koma_eval") != std::string::npos);
            f.koma_search = (s.find("koma_search") != std::string::npos);
            f.tt_static_eval = (s.find("tt_static_eval") != std::string::npos);
            f.fast_probcut = (s.find("fast_probcut") != std::string::npos);
            f.lazy_eval = (s.find("lazy_eval") != std::string::npos);
            f.staged_movegen = (s.find("staged_movegen") != std::string::npos);
            f.drop_history = (s.find("drop_history") != std::string::npos);
            f.koma_search_fast = (s.find("koma_search_fast") != std::string::npos);
            f.siege_eval = (s.find("siege_eval") != std::string::npos);
            f.tapered_v35_eval = (s.find("tapered_v35_eval") != std::string::npos);
            return f;
        }

        if (s.find("-koma_eval") != std::string::npos || s.find("no_koma_eval") != std::string::npos || s.find("koma_eval=0") != std::string::npos) {
            // When disabling koma_eval (reverting to pure v3 baseline), default post-v3 flags to false unless explicitly enabled
            f.tt_static_eval = false;
            f.fast_probcut = false;
            f.lazy_eval = false;
            f.staged_movegen = false;
            f.drop_history = false;
            f.koma_search_fast = false;
            f.siege_eval = false;
            f.tapered_v35_eval = false;
        }
        if (s.find("-siege_eval") != std::string::npos || s.find("no_siege_eval") != std::string::npos || s.find("siege_eval=0") != std::string::npos) {
            f.tapered_v35_eval = false;
        }

        apply_flag(s, "mate_root", f.mate_root);
        apply_flag(s, "mate_threat", f.mate_threat);
        apply_flag(s, "mate_pvlight", f.mate_pvlight);
        apply_flag(s, "mate1_leaf", f.mate1_leaf);
        apply_flag(s, "koma_eval", f.koma_eval);
        apply_flag(s, "koma_search", f.koma_search);
        apply_flag(s, "tt_static_eval", f.tt_static_eval);
        apply_flag(s, "fast_probcut", f.fast_probcut);
        apply_flag(s, "lazy_eval", f.lazy_eval);
        apply_flag(s, "staged_movegen", f.staged_movegen);
        apply_flag(s, "drop_history", f.drop_history);
        apply_flag(s, "koma_search_fast", f.koma_search_fast);
        apply_flag(s, "siege_eval", f.siege_eval);
        apply_flag(s, "tapered_v35_eval", f.tapered_v35_eval);
        apply_flag(s, "own_book", f.own_book);

        return f;
    }
};

} // namespace homei

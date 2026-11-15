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

    static const EngineFeatures& get() {
        static EngineFeatures instance = init();
        return instance;
    }

private:
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
            return f;
        }
        if (s == "all" || s == "1") {
            f.koma_eval = true;
            f.koma_search = true;
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
            return f;
        }
        if (s.find("-mate_root") != std::string::npos || s.find("no_mate_root") != std::string::npos || s.find("mate_root=0") != std::string::npos) f.mate_root = false;
        if (s.find("+mate_root") != std::string::npos || s.find("mate_root=1") != std::string::npos) f.mate_root = true;

        if (s.find("-mate_threat") != std::string::npos || s.find("no_mate_threat") != std::string::npos || s.find("mate_threat=0") != std::string::npos) f.mate_threat = false;
        if (s.find("+mate_threat") != std::string::npos || s.find("mate_threat=1") != std::string::npos) f.mate_threat = true;

        if (s.find("-mate_pvlight") != std::string::npos || s.find("no_mate_pvlight") != std::string::npos || s.find("mate_pvlight=0") != std::string::npos) f.mate_pvlight = false;
        if (s.find("+mate_pvlight") != std::string::npos || s.find("mate_pvlight=1") != std::string::npos) f.mate_pvlight = true;

        if (s.find("-mate1_leaf") != std::string::npos || s.find("no_mate1_leaf") != std::string::npos || s.find("mate1_leaf=0") != std::string::npos) f.mate1_leaf = false;
        if (s.find("+mate1_leaf") != std::string::npos || s.find("mate1_leaf=1") != std::string::npos) f.mate1_leaf = true;

        if (s.find("-koma_eval") != std::string::npos || s.find("no_koma_eval") != std::string::npos || s.find("koma_eval=0") != std::string::npos) f.koma_eval = false;
        if (s.find("+koma_eval") != std::string::npos || s.find("koma_eval=1") != std::string::npos || s.find("koma_eval") != std::string::npos && s.find("-koma_eval") == std::string::npos && s.find("no_koma_eval") == std::string::npos && s.find("koma_eval=0") == std::string::npos) f.koma_eval = true;

        if (s.find("-koma_search") != std::string::npos || s.find("no_koma_search") != std::string::npos || s.find("koma_search=0") != std::string::npos) f.koma_search = false;
        if (s.find("+koma_search") != std::string::npos || s.find("koma_search=1") != std::string::npos || s.find("koma_search") != std::string::npos && s.find("-koma_search") == std::string::npos && s.find("no_koma_search") == std::string::npos && s.find("koma_search=0") == std::string::npos) f.koma_search = true;

        return f;
    }
};

} // namespace homei

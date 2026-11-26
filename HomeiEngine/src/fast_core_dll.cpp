#include "attack.h"
#include "eval.h"
#include "json.h"
#include "lppn.h"
#include "movegen.h"
#include "notation.h"
#include "state_json.h"
#include "types.h"
#include "zobrist.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#ifdef _WIN32
#define CLP_FAST_EXPORT extern "C" __declspec(dllexport)
#else
#define CLP_FAST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

std::once_flag g_fast_init_once;

void ensure_init() {
    std::call_once(g_fast_init_once, []() {
        homei::init_zobrist();
        homei::init_attack_tables();
        (void)homei::get_eval_params();
    });
}

int write_str_buf(const std::string& s, char* out_buf, int buf_size) {
    int need = static_cast<int>(s.size()) + 1;
    if (!out_buf || buf_size <= 0) return need;
    if (buf_size < need) return need;
    std::memcpy(out_buf, s.data(), s.size());
    out_buf[s.size()] = '\0';
    return static_cast<int>(s.size());
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

CLP_FAST_EXPORT const char* clp_fast_version() {
    return "clp_fast_core/1 (Homei v4-4)";
}

CLP_FAST_EXPORT int clp_fast_legal_moves_lppn(const char* lppn, char* out_buf, int buf_size) {
    if (!lppn) return -1;
    ensure_init();
    homei::Position pos;
    int ply = 0;
    std::string err;
    if (!homei::parse_lppn(lppn, pos, ply, err, false)) {
        return -1;
    }
    homei::MoveList list;
    homei::generate_legal_moves(pos, list);
    std::string out;
    out.reserve(static_cast<size_t>(list.count) * 110 + 4);
    out.push_back('[');
    for (int i = 0; i < list.count; ++i) {
        if (i > 0) out.push_back(',');
        std::string not_str = homei::format_canonical_notation(pos, list.moves[i]);
        out += homei::format_move_json_str(list.moves[i], not_str);
    }
    out.push_back(']');
    return write_str_buf(out, out_buf, buf_size);
}

CLP_FAST_EXPORT int clp_fast_apply_lppn(const char* lppn, const char* move_notation, char* out_buf, int buf_size) {
    if (!lppn || !move_notation) return -1;
    ensure_init();
    homei::Position pos;
    int ply = 0;
    std::string err;
    if (!homei::parse_lppn(lppn, pos, ply, err, false)) {
        return -1;
    }
    homei::Move mv;
    if (!homei::parse_canonical_notation(pos, move_notation, mv, err)) {
        return -1;
    }
    homei::UndoState u;
    homei::apply_move(pos, mv, u);
    std::string next_lppn;
    if (!homei::format_lppn(pos, ply + 1, next_lppn, err, false)) {
        return -1;
    }
    homei::Side stm = static_cast<homei::Side>(pos.side_to_move);
    bool chk = homei::is_in_check(pos, stm);
    homei::MoveList leg;
    homei::generate_legal_moves(pos, leg);
    char zbuf[32];
    std::snprintf(zbuf, sizeof(zbuf), "%016llx", static_cast<unsigned long long>(pos.zobrist));
    std::string out = "{\"lppn\":\"" + json_escape(next_lppn) +
                      "\",\"in_check\":" + (chk ? "true" : "false") +
                      ",\"legal_count\":" + std::to_string(leg.count) +
                      ",\"zobrist\":\"" + zbuf + "\"}";
    return write_str_buf(out, out_buf, buf_size);
}

CLP_FAST_EXPORT int clp_fast_eval_lppn(const char* lppn) {
    if (!lppn) return 0;
    ensure_init();
    homei::Position pos;
    int ply = 0;
    std::string err;
    if (!homei::parse_lppn(lppn, pos, ply, err, false)) {
        return 0;
    }
    return homei::evaluate(pos);
}

CLP_FAST_EXPORT int clp_fast_validate_pv_lppn(const char* lppn, const char* pv_notations_json, char* out_buf, int buf_size) {
    if (!lppn || !pv_notations_json) return -1;
    ensure_init();
    homei::Position pos;
    int ply = 0;
    std::string err;
    if (!homei::parse_lppn(lppn, pos, ply, err, false)) {
        return -1;
    }
    homei::JsonValue jv;
    if (!homei::json_parse(pv_notations_json, jv, err) || !jv.is_array()) {
        return -1;
    }
    int ok_count = 0;
    for (const auto& item : jv.arr_val) {
        if (!item.is_string()) break;
        homei::Move mv;
        if (!homei::parse_canonical_notation(pos, item.str_val, mv, err)) {
            break;
        }
        homei::UndoState u;
        homei::apply_move(pos, mv, u);
        ++ply;
        ++ok_count;
    }
    std::string end_lppn;
    (void)homei::format_lppn(pos, ply, end_lppn, err, false);
    std::string out = "{\"valid\":" + std::string(ok_count == static_cast<int>(jv.arr_val.size()) ? "true" : "false") +
                      ",\"applied\":" + std::to_string(ok_count) +
                      ",\"lppn_end\":\"" + json_escape(end_lppn) + "\"}";
    return write_str_buf(out, out_buf, buf_size);
}

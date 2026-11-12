#include "state_json.h"
#include "lppn.h"
#include "zobrist.h"

namespace homei {

bool decode_position_json(const JsonValue& v, Position& out_pos, std::string& err) {
    if (!v.is_object() || v.obj_val.size() != 4) {
        err = "Invalid CLP position fields";
        return false;
    }
    const auto* board_v = v.get("board");
    const auto* to_move_v = v.get("to_move");
    const auto* reserve_v = v.get("reserve");
    const auto* prisoners_v = v.get("prisoners");
    if (!board_v || !to_move_v || !reserve_v || !prisoners_v) {
        err = "Invalid CLP position fields";
        return false;
    }
    if (!board_v->is_array() || board_v->arr_val.size() != NUM_SQUARES) {
        err = "Expected 81 squares";
        return false;
    }
    if (!to_move_v->is_string() || (to_move_v->str_val != "CAM" && to_move_v->str_val != "XANH")) {
        err = "Invalid side to_move";
        return false;
    }

    Position pos;
    pos.clear();
    pos.side_to_move = (to_move_v->str_val == "CAM") ? CAM : XANH;

    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        const JsonValue& cell_v = board_v->arr_val[sq];
        if (!cell_v.is_array()) {
            err = "Invalid cell stack";
            return false;
        }
        if (cell_v.arr_val.size() > MAX_STACK_SIZE) {
            err = "Stack exceeds limit";
            return false;
        }
        Cell& c = pos.cell[sq];
        for (const JsonValue& pv : cell_v.arr_val) {
            if (!pv.is_object() || pv.obj_val.size() != 3) {
                err = "Invalid piece fields";
                return false;
            }
            const auto* t_v = pv.get("type");
            const auto* o_v = pv.get("owner");
            const auto* f_v = pv.get("facing");
            if (!t_v || !o_v || !f_v || !t_v->is_string() || !o_v->is_string() || !f_v->is_int()) {
                err = "Invalid piece fields";
                return false;
            }
            int pt = parse_piece_type_name(t_v->str_val);
            if (pt < 0) {
                err = "Invalid piece type";
                return false;
            }
            if (o_v->str_val != "CAM" && o_v->str_val != "XANH") {
                err = "Invalid piece owner";
                return false;
            }
            if (f_v->int_val < 0 || f_v->int_val > 3) {
                err = "Invalid piece facing";
                return false;
            }
            uint8_t owner = (o_v->str_val == "CAM") ? CAM : XANH;
            c.stack[c.size++] = Piece(static_cast<uint8_t>(pt), owner, static_cast<uint8_t>(f_v->int_val));
        }
    }

    const JsonValue* hands[2] = {reserve_v, prisoners_v};
    for (int hi = 0; hi < 2; ++hi) {
        bool is_res = (hi == 0);
        const JsonValue& hv = *hands[hi];
        if (!hv.is_object() || hv.obj_val.size() != 2) {
            err = "Invalid hand fields";
            return false;
        }
        const auto* cam_arr = hv.get("CAM");
        const auto* xanh_arr = hv.get("XANH");
        if (!cam_arr || !xanh_arr || !cam_arr->is_array() || !xanh_arr->is_array()) {
            err = "Invalid hand fields";
            return false;
        }
        const JsonValue* side_arrs[2] = {cam_arr, xanh_arr};
        for (int s = 0; s < 2; ++s) {
            bool seen_res[8]{};
            for (const JsonValue& item : side_arrs[s]->arr_val) {
                if (!item.is_string()) {
                    err = "Invalid hand item";
                    return false;
                }
                int pt = parse_piece_type_name(item.str_val);
                if (pt < 0 || pt == VUONG) {
                    err = "Invalid hand piece type";
                    return false;
                }
                if (is_res) {
                    if (pt != LONG && pt != PHUONG) {
                        err = "Invalid reserve piece type";
                        return false;
                    }
                    if (seen_res[pt]) {
                        err = "Duplicate reserve piece";
                        return false;
                    }
                    seen_res[pt] = true;
                    pos.set_reserve_type(static_cast<Side>(s), static_cast<uint8_t>(pt), true);
                } else {
                    if (pos.prisoners[s][pt] >= MAX_TOTAL_PIECES) {
                        err = "Too many prisoners";
                        return false;
                    }
                    pos.prisoners[s][pt]++;
                }
            }
        }
    }

    if (!validate_position(pos, true, err)) return false;
    pos.zobrist = compute_zobrist(pos);
    out_pos = pos;
    return true;
}

JsonValue encode_position_json(const Position& pos) {
    JsonValue board_arr = JsonValue::make_array();
    board_arr.arr_val.reserve(NUM_SQUARES);
    for (int sq = 0; sq < NUM_SQUARES; ++sq) {
        JsonValue cell_arr = JsonValue::make_array();
        const Cell& c = pos.cell[sq];
        for (uint8_t i = 0; i < c.size; ++i) {
            const Piece& p = c.stack[i];
            JsonValue po = JsonValue::make_object();
            po.obj_val["facing"] = JsonValue::make_int(p.facing);
            po.obj_val["owner"] = JsonValue::make_string(side_name(static_cast<Side>(p.owner)));
            po.obj_val["type"] = JsonValue::make_string(piece_type_name(p.type));
            cell_arr.arr_val.push_back(std::move(po));
        }
        board_arr.arr_val.push_back(std::move(cell_arr));
    }

    JsonValue res_obj = JsonValue::make_object();
    JsonValue pris_obj = JsonValue::make_object();
    for (int s = 0; s < 2; ++s) {
        const char* sname = side_name(static_cast<Side>(s));
        JsonValue r_arr = JsonValue::make_array();
        if (pos.reserve[s] & RESERVE_LONG) r_arr.arr_val.push_back(JsonValue::make_string("LONG"));
        if (pos.reserve[s] & RESERVE_PHUONG) r_arr.arr_val.push_back(JsonValue::make_string("PHUONG"));
        res_obj.obj_val[sname] = std::move(r_arr);

        JsonValue p_arr = JsonValue::make_array();
        for (int pt = 0; pt < 8; ++pt) {
            for (uint8_t k = 0; k < pos.prisoners[s][pt]; ++k) {
                p_arr.arr_val.push_back(JsonValue::make_string(piece_type_name(static_cast<uint8_t>(pt))));
            }
        }
        pris_obj.obj_val[sname] = std::move(p_arr);
    }

    JsonValue out = JsonValue::make_object();
    out.obj_val["board"] = std::move(board_arr);
    out.obj_val["prisoners"] = std::move(pris_obj);
    out.obj_val["reserve"] = std::move(res_obj);
    out.obj_val["to_move"] = JsonValue::make_string(side_name(static_cast<Side>(pos.side_to_move)));
    return out;
}

bool decode_move_json(const JsonValue& v, Move& out_move, std::string& out_notation, std::string& err) {
    if (!v.is_object() || v.obj_val.size() != 8) {
        err = "Invalid move schema";
        return false;
    }
    const auto* schema_v = v.get("schema");
    const auto* kind_v = v.get("kind");
    const auto* src_v = v.get("src");
    const auto* dst_v = v.get("dst");
    const auto* ptype_v = v.get("ptype");
    const auto* facing_v = v.get("facing");
    const auto* rot_v = v.get("rotate_after");
    const auto* not_v = v.get("notation");
    if (!schema_v || !kind_v || !src_v || !dst_v || !ptype_v || !facing_v || !rot_v || !not_v) {
        err = "Invalid move schema";
        return false;
    }
    if (!schema_v->is_string() || schema_v->str_val != "clp.move/1") {
        err = "Invalid move schema";
        return false;
    }
    if (!kind_v->is_string()) {
        err = "Invalid move kind";
        return false;
    }
    Move m;
    if (kind_v->str_val == "single") m.kind = MoveKind::Single;
    else if (kind_v->str_val == "team") m.kind = MoveKind::Team;
    else if (kind_v->str_val == "rotate") m.kind = MoveKind::Rotate;
    else if (kind_v->str_val == "drop") m.kind = MoveKind::Drop;
    else {
        err = "Invalid move kind";
        return false;
    }

    auto parse_sq = [&](const JsonValue& sv, int8_t& sq_out) -> bool {
        if (sv.is_null()) {
            sq_out = -1;
            return true;
        }
        if (!sv.is_array() || sv.arr_val.size() != 2 || !sv.arr_val[0].is_int() || !sv.arr_val[1].is_int()) {
            return false;
        }
        int64_t r = sv.arr_val[0].int_val;
        int64_t c = sv.arr_val[1].int_val;
        if (!in_bounds(static_cast<int>(r), static_cast<int>(c))) return false;
        sq_out = static_cast<int8_t>(make_sq(static_cast<int>(r), static_cast<int>(c)));
        return true;
    };

    if (!parse_sq(*src_v, m.src) || !parse_sq(*dst_v, m.dst)) {
        err = "Invalid move square";
        return false;
    }
    if (!ptype_v->is_string()) {
        err = "Invalid move ptype";
        return false;
    }
    int pt = parse_piece_type_name(ptype_v->str_val);
    if (pt < 0) {
        err = "Invalid move ptype";
        return false;
    }
    m.ptype = static_cast<uint8_t>(pt);

    if (facing_v->is_null()) {
        m.facing = -1;
    } else if (facing_v->is_int() && facing_v->int_val >= 0 && facing_v->int_val < 4) {
        m.facing = static_cast<int8_t>(facing_v->int_val);
    } else {
        err = "Invalid move facing";
        return false;
    }

    if (!rot_v->is_int() || (rot_v->int_val != 0 && rot_v->int_val != 1 && rot_v->int_val != 3)) {
        err = "Invalid move rotation";
        return false;
    }
    m.rotate_after = static_cast<uint8_t>(rot_v->int_val);

    if (not_v->is_null()) {
        out_notation.clear();
    } else if (not_v->is_string()) {
        out_notation = not_v->str_val;
    } else {
        err = "Invalid move notation";
        return false;
    }

    out_move = m;
    return true;
}

JsonValue encode_move_json(const Move& m, const std::string& notation) {
    JsonValue out = JsonValue::make_object();
    out.obj_val["schema"] = JsonValue::make_string("clp.move/1");
    const char* kstr = "single";
    switch (m.kind) {
        case MoveKind::Single: kstr = "single"; break;
        case MoveKind::Team:   kstr = "team"; break;
        case MoveKind::Rotate: kstr = "rotate"; break;
        case MoveKind::Drop:   kstr = "drop"; break;
    }
    out.obj_val["kind"] = JsonValue::make_string(kstr);

    if (m.src >= 0) {
        JsonValue sa = JsonValue::make_array();
        sa.arr_val.push_back(JsonValue::make_int(sq_row(m.src)));
        sa.arr_val.push_back(JsonValue::make_int(sq_col(m.src)));
        out.obj_val["src"] = std::move(sa);
    } else {
        out.obj_val["src"] = JsonValue::make_null();
    }

    if (m.dst >= 0) {
        JsonValue da = JsonValue::make_array();
        da.arr_val.push_back(JsonValue::make_int(sq_row(m.dst)));
        da.arr_val.push_back(JsonValue::make_int(sq_col(m.dst)));
        out.obj_val["dst"] = std::move(da);
    } else {
        out.obj_val["dst"] = JsonValue::make_null();
    }

    out.obj_val["ptype"] = JsonValue::make_string(piece_type_name(m.ptype));
    if (m.kind == MoveKind::Drop && m.facing >= 0) {
        out.obj_val["facing"] = JsonValue::make_int(m.facing);
    } else {
        out.obj_val["facing"] = JsonValue::make_null();
    }
    out.obj_val["rotate_after"] = JsonValue::make_int(m.rotate_after);
    if (notation.empty()) {
        out.obj_val["notation"] = JsonValue::make_null();
    } else {
        out.obj_val["notation"] = JsonValue::make_string(notation);
    }
    return out;
}

bool decode_state_json(const JsonValue& v, GameState& out_state, std::string& err) {
    if (!v.is_object() || v.obj_val.size() != 6) {
        err = "Invalid state schema";
        return false;
    }
    const auto* schema_v = v.get("schema");
    const auto* pos_v = v.get("position");
    const auto* ply_v = v.get("ply");
    const auto* rep_v = v.get("repetition");
    const auto* res_v = v.get("result");
    const auto* not_v = v.get("notation");
    if (!schema_v || !pos_v || !ply_v || !rep_v || !res_v || !not_v) {
        err = "Missing required state keys";
        return false;
    }
    if (!schema_v->is_string() || schema_v->str_val != "clp.state/1") {
        err = "Unsupported state schema";
        return false;
    }
    if (!ply_v->is_int() || ply_v->int_val < 0 || ply_v->int_val > 1000000) {
        err = "Invalid ply";
        return false;
    }

    GameState st;
    st.ply = static_cast<int>(ply_v->int_val);
    if (not_v->is_null()) {
        st.has_notation = false;
        st.notation.clear();
    } else if (not_v->is_string()) {
        st.has_notation = true;
        st.notation = not_v->str_val;
    } else {
        err = "Invalid notation field";
        return false;
    }

    if (!decode_position_json(*pos_v, st.position, err)) {
        return false;
    }

    if (!rep_v->is_object()) {
        err = "Invalid repetition field";
        return false;
    }
    const auto* comp_v = rep_v->get("complete");
    const auto* entries_v = rep_v->get("entries");
    if (!comp_v || !comp_v->is_bool() || !entries_v || !entries_v->is_array() || entries_v->arr_val.empty()) {
        err = "Invalid repetition entries";
        return false;
    }
    st.repetition_complete = comp_v->bool_val;
    st.history.reserve(entries_v->arr_val.size());

    bool found_current = false;
    for (const JsonValue& ev : entries_v->arr_val) {
        if (!ev.is_object() || ev.obj_val.size() != 2) {
            err = "Invalid repetition entry";
            return false;
        }
        const auto* ep_v = ev.get("position");
        const auto* ec_v = ev.get("count");
        if (!ep_v || !ec_v || !ec_v->is_int() || ec_v->int_val < 1) {
            err = "Invalid repetition entry fields";
            return false;
        }
        RepetitionEntry re;
        if (!decode_position_json(*ep_v, re.position, err)) {
            return false;
        }
        re.hash = re.position.zobrist;
        re.count = static_cast<int>(ec_v->int_val);
        if (re.position.StructuralEqual(st.position)) {
            found_current = true;
        }
        st.history.push_back(std::move(re));
    }
    if (!found_current) {
        err = "Current position missing from repetition.entries";
        return false;
    }

    if (!res_v->is_object()) {
        err = "Invalid result field";
        return false;
    }
    const auto* status_v = res_v->get("status");
    if (!status_v || !status_v->is_string()) {
        err = "Missing result status";
        return false;
    }
    if (status_v->str_val == "ongoing") {
        st.result.status = GameStatus::Ongoing;
    } else if (status_v->str_val == "terminal") {
        st.result.status = GameStatus::Terminal;
        const auto* reason_v = res_v->get("reason");
        if (reason_v && reason_v->is_string()) {
            st.result.reason = reason_v->str_val;
        }
        const auto* win_v = res_v->get("winner");
        if (win_v && win_v->is_string()) {
            st.result.winner = (win_v->str_val == "CAM") ? CAM : XANH;
        }
        const auto* lose_v = res_v->get("loser");
        if (lose_v && lose_v->is_string()) {
            st.result.loser = (lose_v->str_val == "CAM") ? CAM : XANH;
        }
    } else {
        err = "Invalid result status";
        return false;
    }

    out_state = std::move(st);
    return true;
}

JsonValue encode_state_json(const GameState& state) {
    JsonValue out = JsonValue::make_object();
    out.obj_val["schema"] = JsonValue::make_string("clp.state/1");
    out.obj_val["ply"] = JsonValue::make_int(state.ply);
    if (state.has_notation) {
        out.obj_val["notation"] = JsonValue::make_string(state.notation);
    } else {
        out.obj_val["notation"] = JsonValue::make_null();
    }
    out.obj_val["position"] = encode_position_json(state.position);

    JsonValue entries_arr = JsonValue::make_array();
    for (const auto& re : state.history) {
        JsonValue eo = JsonValue::make_object();
        eo.obj_val["count"] = JsonValue::make_int(re.count);
        eo.obj_val["position"] = encode_position_json(re.position);
        entries_arr.arr_val.push_back(std::move(eo));
    }
    JsonValue rep_obj = JsonValue::make_object();
    rep_obj.obj_val["complete"] = JsonValue::make_bool(state.repetition_complete);
    rep_obj.obj_val["entries"] = std::move(entries_arr);
    out.obj_val["repetition"] = std::move(rep_obj);

    JsonValue res_obj = JsonValue::make_object();
    if (state.result.status == GameStatus::Terminal) {
        res_obj.obj_val["status"] = JsonValue::make_string("terminal");
        res_obj.obj_val["reason"] = JsonValue::make_string(state.result.reason);
        res_obj.obj_val["winner"] = (state.result.winner >= 0)
            ? JsonValue::make_string(side_name(static_cast<Side>(state.result.winner)))
            : JsonValue::make_null();
        res_obj.obj_val["loser"] = (state.result.loser >= 0)
            ? JsonValue::make_string(side_name(static_cast<Side>(state.result.loser)))
            : JsonValue::make_null();
    } else {
        res_obj.obj_val["status"] = JsonValue::make_string("ongoing");
    }
    out.obj_val["result"] = std::move(res_obj);
    return out;
}

bool parse_state_json_str(std::string_view json_str, GameState& out_state, std::string& err) {
    JsonValue jv;
    if (!json_parse(json_str, jv, err)) return false;
    return decode_state_json(jv, out_state, err);
}

std::string format_move_json_str(const Move& m, const std::string& notation) {
    return json_stringify(encode_move_json(m, notation));
}

GameState make_base_state(const Position& pos, int ply) {
    GameState st;
    st.position = pos;
    st.position.zobrist = compute_zobrist(st.position);
    st.ply = ply;
    st.repetition_complete = true;
    RepetitionEntry re;
    re.position = st.position;
    re.hash = st.position.zobrist;
    re.count = 1;
    st.history.push_back(std::move(re));
    st.result.status = GameStatus::Ongoing;
    return st;
}

} // namespace homei

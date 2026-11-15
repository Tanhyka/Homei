#pragma once

#include "types.h"
#include <iosfwd>
#include <string>

namespace homei {

enum EvalTermId : int {
    TERM_STACK_BASE = 0,
    TERM_STACK_BATTERY,
    TERM_BINH_REL_ROW,
    TERM_BINH_FORWARD,
    TERM_BINH_GUARD_LONG_KING,
    TERM_BINH_KING_SHIELD_FRONT,
    TERM_BINH_KING_SHIELD_SIDE,
    TERM_TUONG_SI_KI_REL_ROW,
    TERM_CENTER_COL_WEIGHT,
    TERM_STEP_ATTACK_ENEMY,
    TERM_LONG_PHUONG_ACTIVE_RANK,
    TERM_XA_FORWARD,
    // New Koma-gumi midgame deployment terms (active when EngineFeatures::get().koma_eval is true)
    TERM_KOMA_DEAD_ALL_OUT,
    TERM_KOMA_DEAD_PART_VEC,
    TERM_KOMA_DEAD_BINH_HOME,
    TERM_KOMA_BURIED_HOME,
    TERM_KOMA_BURIED_NO_RAY,
    TERM_KOMA_MOBILE_BATTERY,
    TERM_KOMA_SLIDER_MOB_BASE,
    TERM_KOMA_SLIDER_MOB_PRIME,
    TERM_KOMA_XA_REL_ROW,
    TERM_KOMA_UNDEVELOPED_KI_SI,
    EVAL_TERM_COUNT
};

struct EvalParams {
    int w[EVAL_TERM_COUNT];

    static EvalParams defaults() {
        EvalParams p{};
        p.w[TERM_STACK_BASE]              = 10;
        p.w[TERM_STACK_BATTERY]           = 20;
        p.w[TERM_BINH_REL_ROW]            = 3;
        p.w[TERM_BINH_FORWARD]            = 6;
        p.w[TERM_BINH_GUARD_LONG_KING]    = 14;
        p.w[TERM_BINH_KING_SHIELD_FRONT]  = 36;
        p.w[TERM_BINH_KING_SHIELD_SIDE]   = 10;
        p.w[TERM_TUONG_SI_KI_REL_ROW]     = 3;
        p.w[TERM_CENTER_COL_WEIGHT]       = 2;
        p.w[TERM_STEP_ATTACK_ENEMY]       = 12;
        p.w[TERM_LONG_PHUONG_ACTIVE_RANK] = 18;
        p.w[TERM_XA_FORWARD]              = 6;

        p.w[TERM_KOMA_DEAD_ALL_OUT]       = 26;
        p.w[TERM_KOMA_DEAD_PART_VEC]      = 5;
        p.w[TERM_KOMA_DEAD_BINH_HOME]     = 24;
        p.w[TERM_KOMA_BURIED_HOME]        = 47;
        p.w[TERM_KOMA_BURIED_NO_RAY]      = 30;
        p.w[TERM_KOMA_MOBILE_BATTERY]     = 8;
        p.w[TERM_KOMA_SLIDER_MOB_BASE]    = 1;
        p.w[TERM_KOMA_SLIDER_MOB_PRIME]   = 1;
        p.w[TERM_KOMA_XA_REL_ROW]         = 5;
        p.w[TERM_KOMA_UNDEVELOPED_KI_SI]  = 10;
        return p;
    }
};

struct EvalBreakdown {
    int base_fixed = 0;
    int raw[EVAL_TERM_COUNT]{};
    int weight[EVAL_TERM_COUNT]{};
    int contrib[EVAL_TERM_COUNT]{};
    int total = 0;
};

const char* eval_term_name(int term_id);
const EvalParams& get_eval_params();
bool load_eval_params_file(const std::string& path);

// Evaluates position from the perspective of `pos.side_to_move` (in centipawns).
int evaluate(const Position& pos);

// Evaluates position and fills full term-by-term breakdown from `pos.side_to_move` perspective.
EvalBreakdown evaluate_breakdown(const Position& pos);

// Prints `evalterms` breakdown for `pos` to `os`.
void print_evalterms(const Position& pos, std::ostream& os);

// Evaluates position from the perspective of `perspective` (CAM or XANH).
int evaluate_for_side(const Position& pos, Side perspective);

// Symmetry transforms for unit verification (Section 4.3 item 7).
Position mirror_horizontal(const Position& pos);
Position flip_colors_180(const Position& pos);

// Diagnostic piece values query (Search Audit Section 3)
void print_piecevalues(std::ostream& os);

} // namespace homei

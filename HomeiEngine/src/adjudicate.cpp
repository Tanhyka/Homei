#include "adjudicate.h"
#include "attack.h"
#include "movegen.h"
#include "notation.h"

namespace homei {

bool adjudicate_state(GameState& st, std::string& err, bool unavoidable_rep3) {
    const RepetitionEntry* matched = nullptr;
    for (const auto& re : st.history) {
        if (re.position.StructuralEqual(st.position)) {
            matched = &re;
            break;
        }
    }
    if (!matched) {
        err = "Current position missing from repetition history";
        return false;
    }

    Side stm = static_cast<Side>(st.position.side_to_move);
    if (matched->count >= 3) {
        st.result.status = GameStatus::Terminal;
        if (unavoidable_rep3) {
            st.result.reason = "repetition3_draw";
            st.result.winner = -1;
            st.result.loser = -1;
        } else {
            st.result.reason = "repetition3";
            st.result.winner = static_cast<int>(stm);
            st.result.loser = static_cast<int>(other_side(stm));
        }
        return true;
    }

    MoveList legal;
    generate_legal_moves(st.position, legal);
    if (legal.count == 0) {
        bool in_chk = is_in_check(st.position, stm);
        st.result.status = GameStatus::Terminal;
        st.result.reason = in_chk ? "checkmate" : "pat";
        st.result.winner = static_cast<int>(other_side(stm));
        st.result.loser = static_cast<int>(stm);
    } else {
        st.result.status = GameStatus::Ongoing;
        st.result.reason.clear();
        st.result.winner = -1;
        st.result.loser = -1;
    }
    return true;
}

bool apply_move_to_state(GameState& st, const Move& m, std::string& err) {
    if (st.result.status == GameStatus::Terminal) {
        err = "Game is already terminal";
        return false;
    }
    MoveList legal;
    generate_legal_moves(st.position, legal);
    bool is_legal = false;
    for (int i = 0; i < legal.count; ++i) {
        if (legal.moves[i] == m) {
            is_legal = true;
            break;
        }
    }
    if (!is_legal) {
        err = "Illegal move";
        return false;
    }

    bool any_rep2 = false;
    for (const auto& re : st.history) {
        if (re.count >= 2) {
            any_rep2 = true;
            break;
        }
    }

    Position pos_before = st.position;
    UndoState u;
    apply_move(st.position, m, u);
    st.ply += 1;

    bool found = false;
    int new_count = 1;
    for (auto& re : st.history) {
        if (re.position.StructuralEqual(st.position)) {
            re.count += 1;
            new_count = re.count;
            found = true;
            break;
        }
    }
    if (!found) {
        RepetitionEntry re;
        re.position = st.position;
        re.hash = st.position.zobrist;
        re.count = 1;
        st.history.push_back(std::move(re));
    }

    bool unavoidable_rep3 = false;
    if (new_count >= 3 && any_rep2) {
        // Điều 5 (Thiên Nhật Hòa): unavoidable iff every legal move from pos_before leads to a position already at count >= 2
        unavoidable_rep3 = true;
        for (int i = 0; i < legal.count; ++i) {
            if (legal.moves[i] == m) continue;
            UndoState cu;
            apply_move(pos_before, legal.moves[i], cu);
            bool hits_rep3 = false;
            for (const auto& re : st.history) {
                if (re.position.StructuralEqual(pos_before)) {
                    // For the played move's target, its prior count was new_count - 1; for others, re.count
                    int prior_cnt = re.position.StructuralEqual(st.position) ? (new_count - 1) : re.count;
                    if (prior_cnt >= 2) {
                        hits_rep3 = true;
                    }
                    break;
                }
            }
            undo_move(pos_before, legal.moves[i], cu);
            if (!hits_rep3) {
                unavoidable_rep3 = false;
                break;
            }
        }
    }

    return adjudicate_state(st, err, unavoidable_rep3);
}

} // namespace homei

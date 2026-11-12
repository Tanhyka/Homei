#include "adjudicate.h"
#include "attack.h"
#include "movegen.h"
#include "notation.h"

namespace homei {

bool adjudicate_state(GameState& st, std::string& err) {
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
        st.result.reason = "repetition3";
        st.result.winner = static_cast<int>(stm);
        st.result.loser = static_cast<int>(other_side(stm));
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

    UndoState u;
    apply_move(st.position, m, u);
    st.ply += 1;

    bool found = false;
    for (auto& re : st.history) {
        if (re.position.StructuralEqual(st.position)) {
            re.count += 1;
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

    return adjudicate_state(st, err);
}

} // namespace homei

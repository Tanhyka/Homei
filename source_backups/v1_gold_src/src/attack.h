#pragma once

#include "types.h"

namespace homei {

void init_attack_tables();

// Fast reverse-lookup attack check: returns true if any top piece of `by_side`
// has `target_sq` as a raw destination (empty, capture, or own).
bool is_attacked(const Position& pos, int target_sq, Side by_side);

// Returns true if `side`'s VUONG is currently attacked by `other_side(side)`.
inline bool is_in_check(const Position& pos, Side side) {
    uint8_t ksq = pos.king_sq[side];
    if (ksq >= NUM_SQUARES) {
        // Fallback scan if king_sq wasn't cached
        for (int sq = 0; sq < NUM_SQUARES; ++sq) {
            if (!pos.cell[sq].empty() && pos.cell[sq].top().owner == side && pos.cell[sq].top().type == VUONG) {
                return is_attacked(pos, sq, other_side(side));
            }
        }
        return false;
    }
    return is_attacked(pos, ksq, other_side(side));
}

// Populates `attacker_sqs` with all squares containing a top piece of `by_side`
// whose raw_destinations include `target_sq`. Returns count of attackers.
int get_attackers(const Position& pos, int target_sq, Side by_side, int attacker_sqs[NUM_SQUARES]);

struct AttackerList {
    int sq[NUM_SQUARES]{};
    int count = 0;
};

inline AttackerList get_attackers(const Position& pos, int target_sq, Side by_side) {
    AttackerList out;
    out.count = get_attackers(pos, target_sq, by_side, out.sq);
    return out;
}

} // namespace homei

#include "attack.h"
#include "bitboard.h"
#include "geometry.h"

namespace homei {

int get_attackers(const Position& pos, int target_sq, Side by_side, int attacker_sqs[NUM_SQUARES]) {
    Bitboard att = attackers_to_side_bb(pos, target_sq, by_side, pos.bb_occ);
    int count = 0;
    while (att.any()) {
        attacker_sqs[count++] = att.pop_lsb();
    }
    return count;
}

} // namespace homei

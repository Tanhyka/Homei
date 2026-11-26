#include "geometry.h"
#include <atomic>
#include <mutex>

namespace homei {

namespace {

PieceMoveTable g_move_table[8][4];
std::atomic<bool> g_initialized{false};
std::once_flag g_geometry_once;

struct BaseVecList {
    uint8_t count;
    MoveVec vecs[8];
};

constexpr BaseVecList BASE_MOVES[8] = {
    // 0: VUONG
    {8, {{-1, -1, false}, {-1, 0, false}, {-1, 1, false},
         {0, -1, false},                  {0, 1, false},
         {1, -1, false},  {1, 0, false},  {1, 1, false}}},
    // 1: TUONG
    {6, {{-1, 0, false}, {-1, -1, false}, {-1, 1, false},
         {0, -1, false}, {0, 1, false},   {1, 0, false}, {}, {}}},
    // 2: SI
    {5, {{-1, 0, false}, {-1, -1, false}, {-1, 1, false},
         {1, -1, false}, {1, 1, false}, {}, {}, {}}},
    // 3: KI
    {5, {{-2, -1, false}, {-2, 1, false},
         {2, 0, false},   {2, -2, false}, {2, 2, false}, {}, {}, {}}},
    // 4: XA
    {1, {{-1, 0, true}, {}, {}, {}, {}, {}, {}, {}}},
    // 5: LONG
    {4, {{-1, 0, true}, {1, 0, true}, {0, -1, true}, {0, 1, true}, {}, {}, {}, {}}},
    // 6: PHUONG
    {4, {{-1, -1, true}, {-1, 1, true}, {1, -1, true}, {1, 1, true}, {}, {}, {}, {}}},
    // 7: BINH
    {1, {{-1, 0, false}, {}, {}, {}, {}, {}, {}, {}}}
};

} // namespace

void init_geometry() {
    std::call_once(g_geometry_once, []() {
        for (int pt = 0; pt < 8; ++pt) {
            const auto& base = BASE_MOVES[pt];
            for (int f = 0; f < 4; ++f) {
                int eff_f = can_rotate(static_cast<uint8_t>(pt)) ? f : 0;
                g_move_table[pt][f].count = base.count;
                for (int i = 0; i < base.count; ++i) {
                    auto [dr, dc] = rotate_cw(base.vecs[i].dr, base.vecs[i].dc, eff_f);
                    g_move_table[pt][f].vecs[i] = {
                        static_cast<int8_t>(dr),
                        static_cast<int8_t>(dc),
                        base.vecs[i].sliding
                    };
                }
            }
        }
    });
}

const PieceMoveTable& get_piece_moves(uint8_t ptype, uint8_t facing) {
    return g_move_table[ptype & 7][facing & 3];
}

} // namespace homei

#pragma once

#include "book_data.h"
#include "types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace homei::book {

inline constexpr uint32_t pack_move(const Move& m) {
    return ((static_cast<uint32_t>(m.kind) & 3u) << 24)
         | ((static_cast<uint32_t>(static_cast<uint8_t>(m.src)) & 0xFFu) << 16)
         | ((static_cast<uint32_t>(static_cast<uint8_t>(m.dst)) & 0xFFu) << 8)
         | ((static_cast<uint32_t>(m.ptype) & 7u) << 5)
         | ((static_cast<uint32_t>(m.facing + 1) & 7u) << 2)
         | (static_cast<uint32_t>(m.rotate_after) & 3u);
}

inline Move unpack_move(uint32_t p) {
    Move m;
    m.kind = static_cast<MoveKind>((p >> 24) & 3u);
    m.src = static_cast<int8_t>(static_cast<uint8_t>((p >> 16) & 0xFFu));
    m.dst = static_cast<int8_t>(static_cast<uint8_t>((p >> 8) & 0xFFu));
    m.ptype = static_cast<uint8_t>((p >> 5) & 7u);
    m.facing = static_cast<int8_t>(static_cast<int>((p >> 2) & 7u) - 1);
    m.rotate_after = static_cast<uint8_t>(p & 3u);
    return m;
}

struct ProbeOptions {
    bool random_pick = true;
    uint16_t min_weight = 1;
    int score_window = 15;
    uint64_t seed_salt = 0;
};

// External book storage (loaded from .clpbook or GUI .json via BookFile option).
class OpeningBook {
public:
    void clear();
    bool has_external() const { return loaded_external_; }
    size_t entry_count() const;
    const std::string& loaded_path() const { return loaded_path_; }

    // Loads either .clpbook (sorted "key 0xpacked score weight" lines) or GUI .json (LPPN -> move list).
    // Empty path clears external book and reverts to embedded BOOK_ENTRIES[].
    bool load_file(const std::string& path, std::string& err);

    // Rule-safe & rep3-safe lookup: verifies pseudo-legality, king safety, and avoids 3-fold repetition loss.
    bool probe(const GameState& state, Move& out_move, int& out_score, const ProbeOptions& opts = {}) const;

private:
    bool loaded_external_ = false;
    std::string loaded_path_;
    std::vector<BookEntry> external_entries_;
};

// Legacy convenience helper probing embedded BOOK_ENTRIES[] directly with rep3 protection.
bool probe_book(const GameState& state, Move& out_move, int& out_score, const ProbeOptions& opts = {});

} // namespace homei::book

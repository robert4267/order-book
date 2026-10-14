#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// Order id -> slot index. Open addressing with linear probing, sized once up
// front so it never allocates or rehashes while the book is running.
// Replaces std::unordered_map, whose node per entry costs a cache miss on
// every cancel.
class OrderIdMap {
public:
    static constexpr std::uint32_t npos = UINT32_MAX;

    explicit OrderIdMap(std::size_t max_entries) {
        const std::size_t cap = capacity_for(max_entries);
        slots_.assign(cap, Slot{0, npos});
        mask_ = cap - 1;
        shift_ = 64 - std::countr_zero(cap);
    }

    // keep the load factor at or below 0.5 so probe chains stay short
    [[nodiscard]] static constexpr std::size_t capacity_for(std::size_t max_entries) noexcept {
        return std::bit_ceil(std::max<std::size_t>(16, max_entries * 2));
    }
    [[nodiscard]] static constexpr std::size_t slot_bytes() noexcept { return sizeof(Slot); }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] std::uint32_t find(OrderId id) const noexcept {
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.value == npos) return npos;
            if (s.key == id) return s.value;
        }
    }

    // false if the id was already present
    bool insert(OrderId id, std::uint32_t value) {
        std::size_t i = home(id);
        for (; slots_[i].value != npos; i = (i + 1) & mask_) {
            if (slots_[i].key == id) return false;
        }
        slots_[i] = Slot{id, value};
        ++size_;
        return true;
    }

    bool erase(OrderId id) {
        std::size_t hole = home(id);
        for (;; hole = (hole + 1) & mask_) {
            if (slots_[hole].value == npos) return false;
            if (slots_[hole].key == id) break;
        }

        // Backward shift instead of tombstones: pull later entries of the
        // cluster into the hole whenever the hole is on their probe path.
        for (std::size_t j = (hole + 1) & mask_; slots_[j].value != npos; j = (j + 1) & mask_) {
            const std::size_t dist_from_home = (j - home(slots_[j].key)) & mask_;
            const std::size_t dist_from_hole = (j - hole) & mask_;
            if (dist_from_home >= dist_from_hole) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].value = npos;
        --size_;
        return true;
    }

private:
    struct Slot {
        OrderId key;
        std::uint32_t value;
    };

    // Fibonacci hashing: sequential ids spread out well and it's one multiply
    std::size_t home(OrderId id) const noexcept {
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> shift_);
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    int shift_ = 0;
    std::size_t size_ = 0;
};

}  // namespace lob

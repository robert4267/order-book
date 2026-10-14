#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <random>
#include <unordered_map>
#include <vector>

#include "lob/order_id_map.hpp"

using lob::OrderIdMap;

TEST(OrderIdMap, Basics) {
    OrderIdMap m(8);
    EXPECT_EQ(m.find(1), OrderIdMap::npos);
    EXPECT_TRUE(m.insert(1, 10));
    EXPECT_FALSE(m.insert(1, 11));
    EXPECT_EQ(m.find(1), 10u);
    EXPECT_TRUE(m.erase(1));
    EXPECT_FALSE(m.erase(1));
    EXPECT_EQ(m.find(1), OrderIdMap::npos);
    EXPECT_EQ(m.size(), 0u);
}

// Random inserts and erases against std::unordered_map. Running the table
// close to its limit makes long clusters, which is where backward-shift
// deletion bugs show up.
TEST(OrderIdMap, MatchesUnorderedMap) {
    for (std::uint64_t seed = 0; seed < 20; ++seed) {
        constexpr std::size_t kMax = 512;
        OrderIdMap m(kMax);
        std::unordered_map<lob::OrderId, std::uint32_t> ref;
        std::mt19937_64 rng(seed);
        // small key space so erases and re-inserts hit the same keys a lot
        std::uniform_int_distribution<lob::OrderId> key(0, 2000);

        for (int step = 0; step < 50'000; ++step) {
            const lob::OrderId k = key(rng);
            if (rng() % 2 && ref.size() < kMax) {
                const auto v = static_cast<std::uint32_t>(rng() % 100000);
                EXPECT_EQ(m.insert(k, v), ref.emplace(k, v).second);
            } else {
                EXPECT_EQ(m.erase(k), ref.erase(k) == 1);
            }
            ASSERT_EQ(m.size(), ref.size());
        }
        for (lob::OrderId k = 0; k <= 2000; ++k) {
            auto it = ref.find(k);
            ASSERT_EQ(m.find(k), it == ref.end() ? OrderIdMap::npos : it->second) << "key " << k;
        }
    }
}

namespace {

// Same formula as OrderIdMap::home(); finding real collisions this way beats
// guessing at ids and hoping they land in the same slot.
std::size_t home(lob::OrderId id, std::size_t cap) {
    return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> (64 - std::countr_zero(cap)));
}

std::vector<lob::OrderId> ids_with_home(std::size_t slot, std::size_t cap, std::size_t n) {
    std::vector<lob::OrderId> out;
    for (lob::OrderId id = 1; out.size() < n; ++id) {
        if (home(id, cap) == slot) out.push_back(id);
    }
    return out;
}

}  // namespace

TEST(OrderIdMap, ExtremeKeys) {
    OrderIdMap m(4);
    EXPECT_TRUE(m.insert(0, 1));
    EXPECT_TRUE(m.insert(UINT64_MAX, 2));
    EXPECT_EQ(m.find(0), 1u);
    EXPECT_EQ(m.find(UINT64_MAX), 2u);
    EXPECT_TRUE(m.erase(0));
    EXPECT_EQ(m.find(0), OrderIdMap::npos);
    EXPECT_EQ(m.find(UINT64_MAX), 2u);
}

// A cluster that starts in the last slot and wraps to the front. Erasing
// from the middle has to pull the wrapped entries back across the seam.
TEST(OrderIdMap, CollidingIdsAcrossTheWrap) {
    constexpr std::size_t kMax = 8, kCap = 16;  // load factor 0.5 -> 16 slots
    const std::vector<lob::OrderId> last = ids_with_home(kCap - 1, kCap, 4);
    const std::vector<lob::OrderId> first = ids_with_home(0, kCap, 2);

    for (std::size_t victim = 0; victim < last.size(); ++victim) {
        OrderIdMap m(kMax);
        std::uint32_t v = 0;
        for (lob::OrderId id : last) ASSERT_TRUE(m.insert(id, v++));
        for (lob::OrderId id : first) ASSERT_TRUE(m.insert(id, v++));
        ASSERT_TRUE(m.erase(last[victim]));
        EXPECT_EQ(m.size(), last.size() + first.size() - 1);
        for (std::size_t i = 0; i < last.size(); ++i) {
            const auto want = i == victim ? OrderIdMap::npos : static_cast<std::uint32_t>(i);
            EXPECT_EQ(m.find(last[i]), want) << "victim " << victim << " i " << i;
        }
        for (std::size_t i = 0; i < first.size(); ++i) {
            EXPECT_EQ(m.find(first[i]), static_cast<std::uint32_t>(last.size() + i));
        }
        // the freed slot is reusable and nothing else moved under it
        EXPECT_TRUE(m.insert(last[victim], 99));
        EXPECT_EQ(m.find(last[victim]), 99u);
    }
}

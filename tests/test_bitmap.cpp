#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <set>

#include "lob/price_bitmap.hpp"

using lob::PriceBitmap;

namespace {

constexpr std::size_t npos = PriceBitmap::npos;

std::size_t ref_next(const std::set<std::size_t>& s, std::size_t i) {
    auto it = s.lower_bound(i);
    return it == s.end() ? npos : *it;
}

std::size_t ref_prev(const std::set<std::size_t>& s, std::size_t i) {
    auto it = s.upper_bound(i);
    return it == s.begin() ? npos : *std::prev(it);
}

// Sizes around the word (64) and summary (64 * 64 = 4096) boundaries.
constexpr std::size_t kSizes[] = {1, 2, 63, 64, 65, 127, 128, 129, 4095, 4096, 4097, 8193, 300'000};

TEST(PriceBitmap, EmptyAndSingleBitAtEveryEdge) {
    for (std::size_t n : kSizes) {
        PriceBitmap b(n);
        EXPECT_EQ(b.size(), n);
        EXPECT_EQ(b.next(0), npos);
        EXPECT_EQ(b.prev(n - 1), npos);
        EXPECT_EQ(b.prev(npos), npos);

        for (std::size_t i : {std::size_t{0}, n / 2, n - 1}) {
            b.set(i);
            EXPECT_TRUE(b.test(i));
            EXPECT_EQ(b.next(0), i) << "n=" << n;
            EXPECT_EQ(b.next(i), i);
            EXPECT_EQ(b.next(i + 1), npos);
            EXPECT_EQ(b.prev(npos), i) << "n=" << n;
            EXPECT_EQ(b.prev(i), i);
            if (i > 0) {
                EXPECT_EQ(b.prev(i - 1), npos);
            }
            b.clear(i);
            EXPECT_FALSE(b.test(i));
            EXPECT_EQ(b.next(0), npos);
            EXPECT_EQ(b.prev(npos), npos);
        }
        // past the end is never found, even from far away
        EXPECT_EQ(b.next(n), npos);
        EXPECT_EQ(b.next(npos), npos);
    }
}

TEST(PriceBitmap, ClearingOneBitKeepsTheRestOfTheWord) {
    PriceBitmap b(200);
    b.set(64);
    b.set(127);
    b.clear(64);
    EXPECT_EQ(b.next(0), 127u);
    EXPECT_EQ(b.prev(199), 127u);
    b.clear(127);
    EXPECT_EQ(b.next(0), npos);
}

TEST(PriceBitmap, MatchesStdSet) {
    std::mt19937_64 rng(7);
    for (std::size_t n : kSizes) {
        PriceBitmap b(n);
        std::set<std::size_t> ref;
        std::uniform_int_distribution<std::size_t> idx(0, n - 1);
        // fill up, then drain most of it again: the dense phase tests the word
        // masks, the sparse tail has long runs of empty words for the summary
        const int ops = n > 10'000 ? 6000 : 3000;
        for (int step = 0; step < ops; ++step) {
            const bool draining = step > ops / 2;
            const std::size_t i = idx(rng);
            if (!ref.empty() && rng() % 4 < (draining ? 3u : 1u)) {
                const std::size_t next = ref_next(ref, i);
                const std::size_t victim = next == npos ? *ref.begin() : next;
                b.clear(victim);
                ref.erase(victim);
            } else {
                b.set(i);
                ref.insert(i);
            }
            const std::size_t q = idx(rng);
            ASSERT_EQ(b.next(q), ref_next(ref, q)) << "n=" << n << " q=" << q;
            ASSERT_EQ(b.prev(q), ref_prev(ref, q)) << "n=" << n << " q=" << q;
        }
        for (std::size_t q = 0; q < n; q += n > 10'000 ? 97 : 1) {
            ASSERT_EQ(b.test(q), ref.count(q) == 1);
            ASSERT_EQ(b.next(q), ref_next(ref, q)) << "n=" << n << " q=" << q;
            ASSERT_EQ(b.prev(q), ref_prev(ref, q)) << "n=" << n << " q=" << q;
        }
    }
}

}  // namespace

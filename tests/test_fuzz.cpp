#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <random>
#include <vector>

#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"
#include "test_util.hpp"

using namespace lob;

namespace {

// Prices are drawn as center + offset, so a few far-apart centers give
// clusters of levels thousands of ticks apart in a wide book.
struct Scenario {
    Price lo = 1000, hi = 1040;
    std::vector<Price> centers{1020};
    Price spread = 22;  // offsets in [-spread, spread], may land out of range
    std::size_t max_orders = 300;
    OrderId id_space = 400;
    int add = 50, cancel = 25, modify = 17;  // percent, market gets the rest
    bool huge_qty = false;                   // sometimes near the Qty limit
    int seeds = 100, steps = 5000;
    int check_every = 50;  // full validate() walks every level, slow on wide books
};

struct Counts {
    std::size_t trades = 0;
    std::size_t partial_makers = 0;  // trades that left the maker resting
    std::size_t partial_takers = 0;  // adds that traded and then rested the rest
    std::size_t book_full = 0, bad_qty = 0, killed = 0, would_cross = 0;
};

void run(const Scenario& sc, std::uint64_t seed, Counts& counts) {
    OrderBook<TradeLog> fast(sc.lo, sc.hi, sc.max_orders);
    MapOrderBook<TradeLog> ref(sc.lo, sc.hi, sc.max_orders);

    std::mt19937_64 rng(seed);
    auto pick = [&](auto lo, auto hi) {
        return std::uniform_int_distribution<decltype(lo)>(lo, hi)(rng);
    };
    auto pick_qty = [&] {
        if (sc.huge_qty && pick(0, 9) == 0) {
            constexpr Qty kMax = std::numeric_limits<Qty>::max();
            return pick(0, 1) ? kMax - pick(Qty{0}, Qty{60}) : kMax / 2 + pick(Qty{0}, Qty{60});
        }
        return pick(Qty{0}, Qty{60});
    };

    for (int step = 0; step < sc.steps; ++step) {
        const OrderId id = pick(OrderId{1}, sc.id_space);
        const Side side = pick(0, 1) ? Side::Buy : Side::Sell;
        const Price center = sc.centers[pick(std::size_t{0}, sc.centers.size() - 1)];
        const Price price = center + pick(-sc.spread, sc.spread);
        const Qty qty = pick_qty();
        const std::size_t trades_before = ref.listener().trades.size();

        Status a{}, b{};
        const int op = pick(0, 99);
        bool is_add = false;
        if (op < sc.add) {
            const int t = pick(0, 9);  // mostly plain limit orders
            const Tif tif = t < 6 ? Tif::Gtc : static_cast<Tif>(t - 6);
            a = fast.add(id, side, price, qty, tif);
            b = ref.add(id, side, price, qty, tif);
            is_add = true;
        } else if (op < sc.add + sc.cancel) {
            a = fast.cancel(id);
            b = ref.cancel(id);
        } else if (op < sc.add + sc.cancel + sc.modify) {
            a = fast.modify(id, price, qty);
            b = ref.modify(id, price, qty);
        } else {
            a = fast.market(id, side, qty);
            b = ref.market(id, side, qty);
        }

        ASSERT_EQ(a, b) << "seed " << seed << " step " << step;
        ASSERT_EQ(fast.listener().trades, ref.listener().trades)
            << "seed " << seed << " step " << step;
        ASSERT_EQ(fast.order_count(), ref.order_count());
        ASSERT_EQ(fast.best_bid(), ref.best_bid());
        ASSERT_EQ(fast.best_ask(), ref.best_ask());

        const auto& trades = ref.listener().trades;
        counts.trades += trades.size() - trades_before;
        for (std::size_t i = trades_before; i < trades.size(); ++i) {
            if (ref.order_qty(trades[i].maker)) ++counts.partial_makers;
        }
        if (is_add && trades.size() > trades_before && ref.order_qty(id)) ++counts.partial_takers;
        counts.book_full += a == Status::BookFull;
        counts.bad_qty += a == Status::BadQty && qty != 0;
        counts.killed += a == Status::Killed;
        counts.would_cross += a == Status::WouldCross;

        if (step % sc.check_every == 0) {
            ASSERT_TRUE(fast.validate()) << "seed " << seed << " step " << step;
            for (Price c : sc.centers) {
                for (Price p = c - sc.spread; p <= c + sc.spread; ++p) {
                    ASSERT_EQ(fast.depth_at(Side::Buy, p), ref.depth_at(Side::Buy, p));
                    ASSERT_EQ(fast.depth_at(Side::Sell, p), ref.depth_at(Side::Sell, p));
                }
            }
            for (OrderId i = 1; i <= sc.id_space; ++i) {
                ASSERT_EQ(fast.order_qty(i), ref.order_qty(i));
            }
        }
    }
    ASSERT_TRUE(fast.validate());
}

Counts run_all(const Scenario& sc) {
    Counts counts;
    for (int seed = 1; seed <= sc.seeds; ++seed) {
        run(sc, static_cast<std::uint64_t>(seed), counts);
        if (::testing::Test::HasFatalFailure()) break;
    }
    return counts;
}

// The counts guard against a fuzzer that quietly stops exercising anything,
// e.g. after a change to the price band that means nothing ever crosses.
void expect_busy(const Counts& c, const Scenario& sc) {
    const auto ops = static_cast<std::size_t>(sc.seeds) * static_cast<std::size_t>(sc.steps);
    EXPECT_GT(c.trades, ops / 10);
    EXPECT_GT(c.partial_makers, ops / 20);
    EXPECT_GT(c.partial_takers, ops / 100);
}

// Narrow band and small id space: crossing, partial fills, duplicate ids and
// stale cancels are all common.
TEST(Fuzz, MatchesReferenceBook) {
    const Scenario sc;
    const Counts c = run_all(sc);
    expect_busy(c, sc);
    EXPECT_GT(c.killed, 0u);
    EXPECT_GT(c.would_cross, 0u);
}

// Clusters ~300k ticks apart in a million-tick book, so moving the best price
// from one cluster to the next goes through the bitmap's summary layer.
TEST(Fuzz, WideRangeFarApartLevels) {
    Scenario sc;
    sc.lo = -500'000;
    sc.hi = 500'000;
    sc.centers = {-500'000, -170'000, 160'000, 500'000};
    sc.spread = 12;
    sc.add = 60;
    sc.cancel = 20;
    sc.modify = 10;
    sc.seeds = 30;
    sc.check_every = 1000;
    const Counts c = run_all(sc);
    expect_busy(c, sc);
}

TEST(Fuzz, TinyCapacityHitsBookFull) {
    Scenario sc;
    sc.max_orders = 6;
    sc.id_space = 20;
    sc.seeds = 60;
    const Counts c = run_all(sc);
    expect_busy(c, sc);
    EXPECT_GT(c.book_full, static_cast<std::size_t>(sc.seeds) * 50);
}

TEST(Fuzz, ModifyHeavy) {
    Scenario sc;
    sc.add = 25;
    sc.cancel = 5;
    sc.modify = 65;
    sc.seeds = 60;
    const Counts c = run_all(sc);
    expect_busy(c, sc);
}

// Quantities near 2^64 push level totals and FOK liquidity sums to the edge.
TEST(Fuzz, HugeQuantities) {
    Scenario sc;
    sc.huge_qty = true;
    sc.seeds = 40;
    const Counts c = run_all(sc);
    expect_busy(c, sc);
    EXPECT_GT(c.bad_qty, 0u);
}

}  // namespace

// Edge cases and regressions. Typed tests run against both books so the
// reference model can't drift from the fast one on the awkward inputs.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"
#include "test_util.hpp"

using namespace lob;

namespace {

constexpr Qty kMaxQty = std::numeric_limits<Qty>::max();
constexpr Price kMinPrice = std::numeric_limits<Price>::min();
constexpr Price kMaxPrice = std::numeric_limits<Price>::max();

template <class B>
class EdgeTest : public ::testing::Test {
protected:
    B book{100, 200, 64};
    std::vector<Trade>& trades() { return book.listener().trades; }
};

using Books = ::testing::Types<OrderBook<TradeLog>, MapOrderBook<TradeLog>>;
TYPED_TEST_SUITE(EdgeTest, Books);

// --- construction ---

// hi - lo used to overflow, size the level arrays to 0 and crash in add()
TEST(OrderBookCtor, FullInt64RangeIsRejectedNotUB) {
    EXPECT_THROW(OrderBook<>(kMinPrice, kMaxPrice, 16), std::invalid_argument);
    EXPECT_THROW(OrderBook<>(kMinPrice, 0, 16), std::invalid_argument);
    EXPECT_THROW(OrderBook<>(0, std::int64_t{1} << 32, 16), std::invalid_argument);
    EXPECT_THROW(OrderBook<>(0, static_cast<Price>(OrderBook<>::kMaxLevels), 16),
                 std::invalid_argument);
}

TEST(OrderBookCtor, BadArgumentsThrowBeforeAllocating) {
    EXPECT_THROW(OrderBook<>(10, 9, 16), std::invalid_argument);
    EXPECT_THROW(OrderBook<>(0, 10, 0), std::invalid_argument);
    EXPECT_THROW(OrderBook<>(0, 10, UINT32_MAX), std::invalid_argument);
    // would be bad_alloc or length_error if the vectors were sized first
    EXPECT_THROW(OrderBook<>(0, 10, SIZE_MAX), std::invalid_argument);
    EXPECT_THROW(OrderBook<>(kMinPrice, kMaxPrice, SIZE_MAX), std::invalid_argument);

    EXPECT_THROW(MapOrderBook<>(10, 9, 16), std::invalid_argument);
    EXPECT_THROW(MapOrderBook<>(0, 10, 0), std::invalid_argument);
}

TYPED_TEST(EdgeTest, SingleTickRange) {
    TypeParam b(7, 7, 4);
    EXPECT_EQ(b.add(1, Side::Buy, 7, 5), Status::Ok);
    EXPECT_EQ(b.add(2, Side::Buy, 8, 5), Status::BadPrice);
    EXPECT_EQ(b.add(3, Side::Sell, 7, 2), Status::Ok);
    EXPECT_EQ(b.market(4, Side::Sell, 10), Status::Ok);
    std::vector<Trade> expected{{3, 1, 7, 2}, {4, 1, 7, 3}};
    EXPECT_EQ(b.listener().trades, expected);
    EXPECT_EQ(b.order_count(), 0u);
}

// index() is p - min; near the int64 limits that has to stay in range
TYPED_TEST(EdgeTest, RangeAtTheBottomOfInt64) {
    TypeParam b(kMinPrice, kMinPrice + 10, 4);
    EXPECT_EQ(b.add(1, Side::Buy, kMinPrice, 3), Status::Ok);
    EXPECT_EQ(b.add(2, Side::Sell, kMinPrice + 10, 3), Status::Ok);
    EXPECT_EQ(b.best_bid(), kMinPrice);
    EXPECT_EQ(b.best_ask(), kMinPrice + 10);
    EXPECT_EQ(b.market(3, Side::Sell, 1), Status::Ok);
    EXPECT_EQ(b.market(4, Side::Buy, 1), Status::Ok);
    std::vector<Trade> expected{{3, 1, kMinPrice, 1}, {4, 2, kMinPrice + 10, 1}};
    EXPECT_EQ(b.listener().trades, expected);
    EXPECT_EQ(b.depth_at(Side::Buy, kMinPrice), 2u);
}

TYPED_TEST(EdgeTest, RangeAtTheTopOfInt64) {
    TypeParam b(kMaxPrice - 10, kMaxPrice, 4);
    EXPECT_EQ(b.add(1, Side::Sell, kMaxPrice, 3), Status::Ok);
    EXPECT_EQ(b.add(2, Side::Buy, kMaxPrice, 5), Status::Ok);
    EXPECT_EQ(b.best_bid(), kMaxPrice);
    EXPECT_FALSE(b.best_ask());
    EXPECT_EQ(b.market(3, Side::Sell, 1), Status::Ok);
    EXPECT_EQ(b.order_qty(2), 1u);
}

// --- quantity overflow ---

// depth_at(150) used to read 0 here, with two orders resting on it
TYPED_TEST(EdgeTest, LevelQtyOverflowIsBadQtyNotWrap) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 150, kMaxQty), Status::Ok);
    EXPECT_EQ(b.add(2, Side::Buy, 150, 1), Status::BadQty);
    EXPECT_EQ(b.add(3, Side::Buy, 150, 1, Tif::PostOnly), Status::BadQty);
    EXPECT_EQ(b.depth_at(Side::Buy, 150), kMaxQty);
    EXPECT_EQ(b.order_count(), 1u);
    // other levels and the other side are unaffected
    EXPECT_EQ(b.add(4, Side::Buy, 149, kMaxQty), Status::Ok);
    EXPECT_EQ(b.add(5, Side::Sell, 151, kMaxQty), Status::Ok);
}

TYPED_TEST(EdgeTest, ModifyThatWouldOverflowLevelLeavesOrderAlone) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 150, kMaxQty - 10), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 150, 5), Status::Ok);
    ASSERT_EQ(b.add(3, Side::Buy, 140, 5), Status::Ok);

    EXPECT_EQ(b.modify(2, 150, 11), Status::BadQty);  // same level, grows past the limit
    EXPECT_EQ(b.modify(3, 150, 11), Status::BadQty);  // moves into the full level
    EXPECT_EQ(b.order_qty(2), 5u);
    EXPECT_EQ(b.order_qty(3), 5u);
    EXPECT_EQ(b.depth_at(Side::Buy, 140), 5u);

    // exactly at the limit is fine, and growing the order alone in its level too
    EXPECT_EQ(b.modify(2, 150, 10), Status::Ok);
    EXPECT_EQ(b.depth_at(Side::Buy, 150), kMaxQty);
    EXPECT_EQ(b.modify(3, 140, kMaxQty), Status::Ok);

    // order 2 kept its place behind order 1 through the rejected modify
    EXPECT_EQ(b.market(9, Side::Sell, kMaxQty), Status::Ok);
    ASSERT_EQ(this->trades().size(), 2u);
    EXPECT_EQ(this->trades()[0].maker, 1u);
    EXPECT_EQ(this->trades()[1].maker, 2u);
}

// FOK used to add up the available qty without checking for wrap
TYPED_TEST(EdgeTest, FokLiquiditySumDoesNotWrap) {
    auto& b = this->book;
    constexpr Qty kHalf = Qty{1} << 63;
    ASSERT_EQ(b.add(1, Side::Sell, 150, kHalf), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 151, kHalf), Status::Ok);
    EXPECT_EQ(b.add(3, Side::Buy, 151, kMaxQty, Tif::Fok), Status::Ok);
    std::vector<Trade> expected{{3, 1, 150, kHalf}, {3, 2, 151, kHalf - 1}};
    EXPECT_EQ(this->trades(), expected);
    EXPECT_EQ(b.order_qty(2), 1u);
}

TYPED_TEST(EdgeTest, IocAndFokSkipTheLevelLimit) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 150, kMaxQty), Status::Ok);
    EXPECT_EQ(b.add(2, Side::Buy, 150, kMaxQty, Tif::Ioc), Status::Ok);
    EXPECT_EQ(b.order_count(), 0u);
}

// --- time in force ---

TYPED_TEST(EdgeTest, PostOnlyAtTheOppositeBestIsRejected) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 150, 5), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 155, 5), Status::Ok);
    EXPECT_EQ(b.add(3, Side::Sell, 150, 1, Tif::PostOnly), Status::WouldCross);
    EXPECT_EQ(b.add(4, Side::Buy, 155, 1, Tif::PostOnly), Status::WouldCross);
    EXPECT_EQ(b.add(5, Side::Sell, 152, 1, Tif::PostOnly), Status::Ok);
    EXPECT_EQ(b.add(6, Side::Buy, 151, 1, Tif::PostOnly), Status::Ok);
    EXPECT_TRUE(this->trades().empty());
}

TYPED_TEST(EdgeTest, FokCountsLiquidityAtItsLimitButNotBeyond) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 150, 3), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 152, 3), Status::Ok);
    ASSERT_EQ(b.add(3, Side::Sell, 153, 100), Status::Ok);
    EXPECT_EQ(b.add(4, Side::Buy, 152, 7, Tif::Fok), Status::Killed);
    EXPECT_EQ(b.add(5, Side::Buy, 152, 6, Tif::Fok), Status::Ok);
    std::vector<Trade> expected{{5, 1, 150, 3}, {5, 2, 152, 3}};
    EXPECT_EQ(this->trades(), expected);
    EXPECT_EQ(b.best_ask(), 153);
}

TYPED_TEST(EdgeTest, IocAndFokStillRejectDuplicateIds) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 150, 3), Status::Ok);
    EXPECT_EQ(b.add(1, Side::Buy, 150, 3, Tif::Ioc), Status::DuplicateId);
    EXPECT_EQ(b.add(1, Side::Buy, 150, 3, Tif::Fok), Status::DuplicateId);
    EXPECT_TRUE(this->trades().empty());
}

// --- full book ---

TYPED_TEST(EdgeTest, FullBookStillTakesOrdersThatNeverRest) {
    TypeParam b(0, 10, 2);
    ASSERT_EQ(b.add(1, Side::Sell, 5, 3), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 6, 3), Status::Ok);
    // would rest 1 after filling, so it's rejected whole with nothing traded
    EXPECT_EQ(b.add(3, Side::Buy, 6, 7), Status::BookFull);
    EXPECT_TRUE(b.listener().trades.empty());
    // fills completely, so the full book doesn't matter
    EXPECT_EQ(b.add(4, Side::Buy, 6, 4), Status::Ok);
    std::vector<Trade> expected{{4, 1, 5, 3}, {4, 2, 6, 1}};
    EXPECT_EQ(b.listener().trades, expected);
    EXPECT_EQ(b.order_count(), 1u);
}

TYPED_TEST(EdgeTest, ModifyOnAFullBook) {
    TypeParam b(0, 10, 2);
    ASSERT_EQ(b.add(1, Side::Buy, 4, 3), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 8, 3), Status::Ok);
    // replace frees its own slot first, so it never sees the book as full
    EXPECT_EQ(b.modify(1, 5, 10), Status::Ok);
    EXPECT_EQ(b.modify(2, 7, 1), Status::Ok);
    EXPECT_EQ(b.best_bid(), 5);
    EXPECT_EQ(b.best_ask(), 7);
}

// --- modify ---

TYPED_TEST(EdgeTest, ModifyThatFullyFillsRemovesTheId) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 155, 10), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Buy, 150, 4), Status::Ok);
    EXPECT_EQ(b.modify(2, 155, 4), Status::Ok);
    std::vector<Trade> expected{{2, 1, 155, 4}};
    EXPECT_EQ(this->trades(), expected);
    EXPECT_FALSE(b.order_qty(2));
    EXPECT_EQ(b.cancel(2), Status::UnknownId);
    EXPECT_EQ(b.modify(2, 150, 1), Status::UnknownId);
    EXPECT_EQ(b.order_qty(1), 6u);
}

TYPED_TEST(EdgeTest, ModifyToSameQtyKeepsPriority) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 150, 5), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 150, 5), Status::Ok);
    EXPECT_EQ(b.modify(1, 150, 5), Status::Ok);
    EXPECT_EQ(b.market(3, Side::Buy, 1), Status::Ok);
    ASSERT_EQ(this->trades().size(), 1u);
    EXPECT_EQ(this->trades()[0].maker, 1u);
}

TYPED_TEST(EdgeTest, ModifySideNeverChanges) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Buy, 150, 5), Status::Ok);
    EXPECT_EQ(b.modify(1, 160, 5), Status::Ok);
    EXPECT_EQ(b.best_bid(), 160);
    EXPECT_FALSE(b.best_ask());
}

// --- market orders ---

// a market order's id is only a tag; reusing a resting id trades against it
TYPED_TEST(EdgeTest, MarketIdMayMatchARestingId) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 150, 5), Status::Ok);
    EXPECT_EQ(b.market(1, Side::Buy, 2), Status::Ok);
    std::vector<Trade> expected{{1, 1, 150, 2}};
    EXPECT_EQ(this->trades(), expected);
    EXPECT_EQ(b.order_qty(1), 3u);
}

TYPED_TEST(EdgeTest, MarketSweepsToTheFarEdge) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 101, 1), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 200, 1), Status::Ok);
    ASSERT_EQ(b.add(3, Side::Buy, 100, 1), Status::Ok);
    EXPECT_EQ(b.market(4, Side::Buy, 5), Status::Ok);
    EXPECT_FALSE(b.best_ask());
    EXPECT_EQ(b.market(5, Side::Sell, 5), Status::Ok);
    EXPECT_FALSE(b.best_bid());
    std::vector<Trade> expected{{4, 1, 101, 1}, {4, 2, 200, 1}, {5, 3, 100, 1}};
    EXPECT_EQ(this->trades(), expected);
}

// Worked out by hand, independent of either implementation, so the reference
// book isn't only trusted because it agrees with the fast one.
TYPED_TEST(EdgeTest, HandComputedSession) {
    auto& b = this->book;
    ASSERT_EQ(b.add(1, Side::Sell, 152, 10), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 151, 5), Status::Ok);
    ASSERT_EQ(b.add(3, Side::Sell, 151, 7), Status::Ok);
    ASSERT_EQ(b.add(4, Side::Buy, 149, 20), Status::Ok);
    ASSERT_EQ(b.add(5, Side::Buy, 150, 4), Status::Ok);
    // asks: 151 [2:5, 3:7], 152 [1:10]   bids: 150 [5:4], 149 [4:20]

    ASSERT_EQ(b.add(6, Side::Buy, 152, 15), Status::Ok);  // 5 + 7 at 151, then 3 of order 1
    ASSERT_EQ(b.add(7, Side::Sell, 152, 2), Status::Ok);
    ASSERT_EQ(b.modify(1, 152, 8), Status::Ok);            // 7 left, grows to 8: behind order 7
    ASSERT_EQ(b.add(8, Side::Sell, 149, 30), Status::Ok);  // 4 at 150, 20 at 149, rests 6
    // asks: 149 [8:6], 152 [7:2, 1:8]   bids: none
    ASSERT_EQ(b.market(9, Side::Buy, 10), Status::Ok);

    const std::vector<Trade> expected{
        {6, 2, 151, 5}, {6, 3, 151, 7},  {6, 1, 152, 3},  //
        {8, 5, 150, 4}, {8, 4, 149, 20},                  //
        {9, 8, 149, 6}, {9, 7, 152, 2},  {9, 1, 152, 2},
    };
    EXPECT_EQ(this->trades(), expected);
    EXPECT_FALSE(b.best_bid());
    EXPECT_EQ(b.best_ask(), 152);
    EXPECT_EQ(b.order_qty(1), 6u);
    EXPECT_EQ(b.depth_at(Side::Sell, 152), 6u);
    EXPECT_EQ(b.order_count(), 1u);
}

struct ThrowOnSecondTrade {
    int seen = 0;
    void on_trade(const Trade&) {
        if (++seen == 2) throw std::runtime_error("listener failed");
    }
};

template <class B>
class ListenerTest : public ::testing::Test {};
using ThrowingBooks =
    ::testing::Types<OrderBook<ThrowOnSecondTrade>, MapOrderBook<ThrowOnSecondTrade>>;
TYPED_TEST_SUITE(ListenerTest, ThrowingBooks);

// A throwing listener leaves the book valid: the reported trade is applied
// and the rest of the taker is dropped.
TYPED_TEST(ListenerTest, ThrowingListenerLeavesBookConsistent) {
    TypeParam b(100, 200, 16);
    ASSERT_EQ(b.add(1, Side::Sell, 150, 2), Status::Ok);
    ASSERT_EQ(b.add(2, Side::Sell, 151, 2), Status::Ok);
    ASSERT_EQ(b.add(3, Side::Sell, 152, 2), Status::Ok);
    EXPECT_THROW(static_cast<void>(b.add(4, Side::Buy, 152, 5)), std::runtime_error);

    EXPECT_FALSE(b.order_qty(1));
    EXPECT_FALSE(b.order_qty(2));  // its trade was reported when the listener threw
    EXPECT_EQ(b.order_qty(3), 2u);
    EXPECT_FALSE(b.order_qty(4));
    EXPECT_EQ(b.best_ask(), 152);
    EXPECT_EQ(b.order_count(), 1u);
    if constexpr (requires { b.validate(); }) {
        EXPECT_TRUE(b.validate());
    }
}

}  // namespace
